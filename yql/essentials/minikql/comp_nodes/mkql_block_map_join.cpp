#include "mkql_block_map_join.h"

#include <yql/essentials/minikql/computation/mkql_block_builder.h>
#include <yql/essentials/minikql/computation/mkql_block_impl.h>
#include <yql/essentials/minikql/computation/mkql_block_reader.h>
#include <yql/essentials/minikql/computation/mkql_block_trimmer.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders_codegen.h>
#include <yql/essentials/minikql/comp_nodes/mkql_rh_hash.h>
#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>
#include <yql/essentials/minikql/mkql_block_map_join_utils.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_program_builder.h>

#include <util/generic/serialized_enum.h>

namespace NKikimr {
namespace NMiniKQL {

namespace {

size_t CalcMaxBlockLength(const TVector<TType*>& items) {
    return CalcBlockLen(std::accumulate(items.cbegin(), items.cend(), 0ULL,
        [](size_t max, const TType* type) {
            const TType* itemType = AS_TYPE(TBlockType, type)->GetItemType();
            return std::max(max, CalcMaxBlockItemSize(itemType));
        }));
}

TMaybe<ui64> CalculateTupleHash(const std::vector<NYql::NUdf::TBlockItem>& items, const std::vector<ui64>& hashes) {
    ui64 hash = 0;
    for (size_t i = 0; i < hashes.size(); i++) {
        if (!items[i]) {
            return {};
        }

        hash = CombineHashes(hash, hashes[i]);
    }

    return hash;
}

template <bool RightRequired>
class TBlockJoinState : public TBlockState {
public:
    TBlockJoinState(TMemoryUsageInfo* memInfo, TComputationContext& ctx,
                    const TVector<TType*>& inputItems,
                    const TVector<ui32>& leftIOMap,
                    const TVector<TType*> outputItems)
        : TBlockState(memInfo, outputItems.size())
        , InputWidth_(inputItems.size() - 1)
        , OutputWidth_(outputItems.size() - 1)
        , Inputs_(inputItems.size())
        , LeftIOMap_(leftIOMap)
        , InputsDescr_(ToValueDescr(inputItems))
    {
        const auto& pgBuilder = ctx.Builder->GetPgBuilder();
        MaxLength_ = CalcMaxBlockLength(outputItems);
        TBlockTypeHelper helper;
        for (size_t i = 0; i < inputItems.size(); i++) {
            TType* blockItemType = AS_TYPE(TBlockType, inputItems[i])->GetItemType();
            Readers_.push_back(MakeBlockReader(TTypeInfoHelper(), blockItemType));
            Converters_.push_back(MakeBlockItemConverter(TTypeInfoHelper(), blockItemType, pgBuilder));
            Hashers_.push_back(helper.MakeHasher(blockItemType));
        }
        // The last output column (i.e. block length) doesn't require a block builder.
        for (size_t i = 0; i < OutputWidth_; i++) {
            const TType* blockItemType = AS_TYPE(TBlockType, outputItems[i])->GetItemType();
            Builders_.push_back(MakeArrayBuilder(TTypeInfoHelper(), blockItemType, ctx.ArrowMemoryPool, MaxLength_, &pgBuilder, &BuilderAllocatedSize_));
        }
        MaxBuilderAllocatedSize_ = MaxAllocatedFactor_ * BuilderAllocatedSize_;
    }

    void CopyRow() {
        // Copy items from the "left" stream.
        // Use the mapping from input fields to output ones to
        // produce a tight loop to copy row items.
        for (size_t i = 0; i < LeftIOMap_.size(); i++) {
            AddItem(GetItem(LeftIOMap_[i]), i);
        }
        OutputRows_++;
    }

    void MakeRow(const NUdf::TUnboxedValuePod& value) {
        size_t builderIndex = 0;
        // Copy items from the "left" stream.
        // Use the mapping from input fields to output ones to
        // produce a tight loop to copy row items.
        for (size_t i = 0; i < LeftIOMap_.size(); i++, builderIndex++) {
            AddItem(GetItem(LeftIOMap_[i]), i);
        }
        // Convert and append items from the "right" dict.
        // Since the keys are copied to the output only from the
        // "left" stream, process all values unconditionally.
        if constexpr (RightRequired) {
            for (size_t i = 0; builderIndex < OutputWidth_; i++) {
                AddValue(value.GetElement(i), builderIndex++);
            }
        } else {
            if (value) {
                for (size_t i = 0; builderIndex < OutputWidth_; i++) {
                    AddValue(value.GetElement(i), builderIndex++);
                }
            } else {
                while (builderIndex < OutputWidth_) {
                    AddValue(value, builderIndex++);
                }
            }
        }
        OutputRows_++;
    }

    void MakeRow(const std::vector<NYql::NUdf::TBlockItem>& rightColumns) {
        size_t builderIndex = 0;

        for (size_t i = 0; i < LeftIOMap_.size(); i++, builderIndex++) {
            AddItem(GetItem(LeftIOMap_[i]), builderIndex);
        }

        if (!rightColumns.empty()) {
            Y_ENSURE(LeftIOMap_.size() + rightColumns.size() == OutputWidth_);
            for (size_t i = 0; i < rightColumns.size(); i++) {
                AddItem(rightColumns[i], builderIndex++);
            }
        } else {
            while (builderIndex < OutputWidth_) {
                AddItem(TBlockItem(), builderIndex++);
            }
        }

        OutputRows_++;
    }

    void MakeBlocks(const THolderFactory& holderFactory) {
        Values.back() = holderFactory.CreateArrowBlock(arrow::Datum(std::make_shared<arrow::UInt64Scalar>(OutputRows_)));
        OutputRows_ = 0;
        BuilderAllocatedSize_ = 0;

        for (size_t i = 0; i < Builders_.size(); i++) {
            Values[i] = holderFactory.CreateArrowBlock(Builders_[i]->Build(IsFinished_));
        }
        FillArrays();
    }

    TBlockItem GetItem(size_t idx, size_t offset = 0) const {
        Y_ENSURE(Current_ + offset < InputRows_);
        const auto& datum = TArrowBlock::From(Inputs_[idx]).GetDatum();
        ARROW_DEBUG_CHECK_DATUM_TYPES(InputsDescr_[idx], datum.descr());
        if (datum.is_scalar()) {
            return Readers_[idx]->GetScalarItem(*datum.scalar());
        }
        MKQL_ENSURE(datum.is_array(), "Expecting array");
        return Readers_[idx]->GetItem(*datum.array(), Current_ + offset);
    }

    std::pair<TBlockItem, ui64> GetItemWithHash(size_t idx, size_t offset) const {
        auto item = GetItem(idx, offset);
        ui64 hash = Hashers_[idx]->Hash(item);
        return std::make_pair(item, hash);
    }

    NUdf::TUnboxedValuePod GetValue(const THolderFactory& holderFactory, size_t idx) const {
        return Converters_[idx]->MakeValue(GetItem(idx), holderFactory);
    }

    void Reset() {
        Current_ = 0;
        InputRows_ = GetBlockCount(Inputs_.back());
    }

    void Finish() {
        IsFinished_ = true;
    }

    void NextRow() {
        Current_++;
    }

    bool HasBlocks() {
        return Count > 0;
    }

    bool IsNotFull() const {
        return OutputRows_ < MaxLength_
            && BuilderAllocatedSize_ <= MaxBuilderAllocatedSize_;
    }

    bool IsEmpty() const {
        return OutputRows_ == 0;
    }

    bool IsFinished() const {
        return IsFinished_;
    }

    size_t RemainingRowsCount() const {
        Y_ENSURE(InputRows_ >= Current_);
        return InputRows_ - Current_;
    }

    NUdf::TUnboxedValue* GetRawInputFields() {
        return Inputs_.data();
    }

    size_t GetInputWidth() const {
        // Mind the last block length column.
        return InputWidth_ + 1;
    }

    size_t GetOutputWidth() const {
        // Mind the last block length column.
        return OutputWidth_ + 1;
    }

private:
    void AddItem(const TBlockItem& item, size_t idx) {
        Builders_[idx]->Add(item);
    }

    void AddValue(const NUdf::TUnboxedValuePod& value, size_t idx) {
        Builders_[idx]->Add(value);
    }

    size_t Current_ = 0;
    bool IsFinished_ = false;
    size_t MaxLength_;
    size_t BuilderAllocatedSize_ = 0;
    size_t MaxBuilderAllocatedSize_ = 0;
    static const size_t MaxAllocatedFactor_ = 4;
    size_t InputRows_ = 0;
    size_t OutputRows_ = 0;
    size_t InputWidth_;
    size_t OutputWidth_;
    TUnboxedValueVector Inputs_;
    const TVector<ui32> LeftIOMap_;
    const std::vector<arrow::ValueDescr> InputsDescr_;
    TVector<std::unique_ptr<IBlockReader>> Readers_;
    TVector<std::unique_ptr<IBlockItemConverter>> Converters_;
    TVector<std::unique_ptr<IArrayBuilder>> Builders_;
    TVector<NYql::NUdf::IBlockItemHasher::TPtr> Hashers_;
};

class TBlockStorage : public TComputationValue<TBlockStorage> {
    using TBase = TComputationValue<TBlockStorage>;

public:
    struct TBlock {
        size_t Size;
        std::vector<arrow::Datum> Columns;

        TBlock() = default;
        TBlock(size_t size, std::vector<arrow::Datum> columns)
            : Size(size)
            , Columns(std::move(columns))
        {}
    };

    struct TRowEntry {
        ui32 BlockOffset;
        ui32 ItemOffset;

        TRowEntry() = default;
        TRowEntry(ui32 blockOffset, ui32 itemOffset)
            : BlockOffset(blockOffset)
            , ItemOffset(itemOffset)
        {}
    };

    class TRowIterator {
        friend class TBlockStorage;

    public:
        TRowIterator() = default;

        TRowIterator(const TRowIterator&) = default;
        TRowIterator& operator=(const TRowIterator&) = default;

        TMaybe<TRowEntry> Next() {
            Y_ENSURE(IsValid());
            if (IsEmpty()) {
                return Nothing();
            }

            auto entry = TRowEntry(CurrentBlockOffset_, CurrentItemOffset_);

            auto& block = BlockStorage_->GetBlock(CurrentBlockOffset_);
            CurrentItemOffset_++;
            if (CurrentItemOffset_ == block.Size) {
                CurrentBlockOffset_++;
                CurrentItemOffset_ = 0;
            }

            return entry;
        }

        bool IsValid() const {
            return BlockStorage_;
        }

        bool IsEmpty() const {
            Y_ENSURE(IsValid());
            return CurrentBlockOffset_ >= BlockStorage_->GetBlockCount();
        }

    private:
        TRowIterator(const TBlockStorage* blockStorage)
            : BlockStorage_(blockStorage)
        {}

    private:
        size_t CurrentBlockOffset_ = 0;
        size_t CurrentItemOffset_ = 0;

        const TBlockStorage* BlockStorage_ = nullptr;
    };

    TBlockStorage(
        TMemoryUsageInfo* memInfo,
        const TVector<TType*>& types,
        size_t blockLengthIndex,
        NUdf::TUnboxedValue listIter,
        TStringBuf resourceTag,
        arrow::MemoryPool* pool
    )
        : TBase(memInfo)
        , InputsDescr_(ToValueDescr(types))
        , Readers_(types.size())
        , Hashers_(types.size())
        , Comparators_(types.size())
        , Trimmers_(types.size())
        , ListIter_(std::move(listIter))
        , BlockLengthIndex_(blockLengthIndex)
        , ResourceTag_(std::move(resourceTag))
    {
        TBlockTypeHelper helper;
        for (size_t i = 0; i < types.size(); i++) {
            if (i == BlockLengthIndex_) {
                continue;
            }

            TType* blockItemType = AS_TYPE(TBlockType, types[i])->GetItemType();
            Readers_[i] = MakeBlockReader(TTypeInfoHelper(), blockItemType);
            Hashers_[i] = helper.MakeHasher(blockItemType);
            Comparators_[i] = helper.MakeComparator(blockItemType);
            Trimmers_[i] = MakeBlockTrimmer(TTypeInfoHelper(), blockItemType, pool);
        }
    }

    bool FetchNextBlock() {
        if (!ListIter_.Next(Block_)) {
            IsFinished_ = true;
            return false;
        }
        BlockItems_ = Block_.GetElements();

        Y_ENSURE(!IsFinished_, "Got data on finished stream");

        std::vector<arrow::Datum> blockColumns(Readers_.size());
        for (size_t i = 0; i < Readers_.size(); i++) {
            if (i == BlockLengthIndex_) {
                continue;
            }

            auto& datum = TArrowBlock::From(BlockItems_[i]).GetDatum();
            ARROW_DEBUG_CHECK_DATUM_TYPES(InputsDescr_[i], datum.descr());
            if (datum.is_scalar()) {
                blockColumns[i] = datum;
            } else {
                MKQL_ENSURE(datum.is_array(), "Expecting array");
                blockColumns[i] = Trimmers_[i]->Trim(datum.array());
            }
        }

        auto blockSize = ::GetBlockCount(BlockItems_[BlockLengthIndex_]);
        Data_.emplace_back(blockSize, std::move(blockColumns));
        RowCount_ += blockSize;

        return true;
    }

    const TBlock& GetBlock(size_t blockOffset) const {
        Y_ENSURE(blockOffset < GetBlockCount());
        return Data_[blockOffset];
    }

    size_t GetBlockCount() const {
        return Data_.size();
    }

    TRowEntry GetRowEntry(size_t blockOffset, size_t itemOffset) const {
        auto& block = GetBlock(blockOffset);
        Y_ENSURE(itemOffset < block.Size);
        return TRowEntry(blockOffset, itemOffset);
    }

    TRowIterator GetRowIterator() const {
        return TRowIterator(this);
    }

    size_t GetRowCount() const {
        return RowCount_;
    }

    TBlockItem GetItem(TRowEntry entry, ui32 columnIdx) const {
        return GetItemFromBlock(GetBlock(entry.BlockOffset), columnIdx, entry.ItemOffset);
    }

    TBlockItem GetItemFromBlock(const TBlock& block, ui32 columnIdx, size_t offset) const {
        Y_ENSURE(columnIdx < Readers_.size() && columnIdx != BlockLengthIndex_);
        Y_ENSURE(offset < block.Size);
        const auto& datum = block.Columns[columnIdx];
        if (datum.is_scalar()) {
            return Readers_[columnIdx]->GetScalarItem(*datum.scalar());
        } else {
            MKQL_ENSURE(datum.is_array(), "Expecting array");
            return Readers_[columnIdx]->GetItem(*datum.array(), offset);
        }
    }

    void GetRow(TRowEntry entry, const TVector<ui32>& ioMap, std::vector<NYql::NUdf::TBlockItem>& row) const {
        Y_ENSURE(row.size() == ioMap.size());
        for (size_t i = 0; i < row.size(); i++) {
            row[i] = GetItem(entry, ioMap[i]);
        }
    }

    const TVector<NUdf::IBlockItemComparator::TPtr>& GetItemComparators() const {
        return Comparators_;
    }

    const TVector<NUdf::IBlockItemHasher::TPtr>& GetItemHashers() const {
        return Hashers_;
    }

    bool IsFinished() const {
        return IsFinished_;
    }

private:
    NUdf::TStringRef GetResourceTag() const override {
        return NUdf::TStringRef(ResourceTag_);
    }

    void* GetResource() override {
        return this;
    }

protected:
    const std::vector<arrow::ValueDescr> InputsDescr_;

    TVector<std::unique_ptr<IBlockReader>> Readers_;
    TVector<NUdf::IBlockItemHasher::TPtr> Hashers_;
    TVector<NUdf::IBlockItemComparator::TPtr> Comparators_;
    TVector<IBlockTrimmer::TPtr> Trimmers_;

    std::vector<TBlock> Data_;
    size_t RowCount_ = 0;
    bool IsFinished_ = false;

    NUdf::TUnboxedValue ListIter_;
    NUdf::TUnboxedValue Block_;
    const NUdf::TUnboxedValue* BlockItems_ = nullptr;

    size_t BlockLengthIndex_ = 0;

    const TStringBuf ResourceTag_;
};

class TBlockStorageWrapper : public TMutableComputationNode<TBlockStorageWrapper> {
    using TBaseComputation = TMutableComputationNode<TBlockStorageWrapper>;

public:
    TBlockStorageWrapper(
        TComputationMutables& mutables,
        TStructType* structType,
        IComputationNode* list,
        const TStringBuf& resourceTag
    )
        : TBaseComputation(mutables, EValueRepresentation::Boxed)
        , List_(list)
        , ResourceTag_(resourceTag)
    {
        for (size_t i = 0; i < structType->GetMembersCount(); i++) {
            if (structType->GetMemberName(i) == NYql::BlockLengthColumnName) {
                BlockLengthIndex_ = i;
                Types_.push_back(nullptr);
                continue;
            }
            Types_.push_back(structType->GetMemberType(i));
        }
    }

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        return ctx.HolderFactory.Create<TBlockStorage>(
            Types_,
            BlockLengthIndex_,
            List_->GetValue(ctx).GetListIterator(),
            ResourceTag_,
            &ctx.ArrowMemoryPool
        );
    }

private:
    void RegisterDependencies() const final {
        DependsOn(List_);
    }

private:
    TVector<TType*> Types_;
    size_t BlockLengthIndex_ = 0;

    IComputationNode* const List_;

    const TString ResourceTag_;
};

class TBlockIndex : public TComputationValue<TBlockIndex> {
    using TBase = TComputationValue<TBlockIndex>;

    struct TIndexNode {
        TBlockStorage::TRowEntry Entry;
        TIndexNode* Next;

        TIndexNode() = delete;
        TIndexNode(TBlockStorage::TRowEntry entry, TIndexNode* next = nullptr)
            : Entry(entry)
            , Next(next)
        {}
    };

    class TIndexMapValue {
    public:
        TIndexMapValue()
            : Raw(0)
        {}

        TIndexMapValue(TBlockStorage::TRowEntry entry) {
            TIndexEntryUnion un;
            un.Entry = entry;

            Y_ENSURE(((un.Raw << 1) >> 1) == un.Raw);
            Raw = (un.Raw << 1) | 1;
        }

        TIndexMapValue(TIndexNode* entryList)
            : EntryList(entryList)
        {}

        bool IsInplace() const {
            return Raw & 1;
        }

        TIndexNode* GetList() const {
            Y_ENSURE(!IsInplace());
            return EntryList;
        }

        TBlockStorage::TRowEntry GetEntry() const {
            Y_ENSURE(IsInplace());

            TIndexEntryUnion un;
            un.Raw = Raw >> 1;
            return un.Entry;
        }

    private:
        union TIndexEntryUnion {
            TBlockStorage::TRowEntry Entry;
            ui64 Raw;
        };

        union {
            TIndexNode* EntryList;
            ui64 Raw;
        };
    };

    using TIndexMap = TRobinHoodHashFixedMap<
        ui64,
        TIndexMapValue,
        std::equal_to<ui64>,
        std::hash<ui64>,
        TMKQLHugeAllocator<char>
    >;

    static_assert(sizeof(TIndexMapValue) == 8);
    static_assert(std::max(TIndexMap::GetCellSize(), static_cast<ui32>(sizeof(TIndexNode))) == BlockMapJoinIndexEntrySize);

public:
    class TIterator {
        friend class TBlockIndex;

        enum class EIteratorType {
            EMPTY,
            INPLACE,
            LIST
        };

    public:
        TIterator() = default;

        TIterator(const TIterator&) = delete;
        TIterator& operator=(const TIterator&) = delete;

        TIterator(TIterator&& other) {
            *this = std::move(other);
        }

        TIterator& operator=(TIterator&& other) {
            if (this != &other) {
                Type_ = other.Type_;
                BlockIndex_ = other.BlockIndex_;
                ItemsToLookup_ = std::move(other.ItemsToLookup_);

                switch (Type_) {
                case EIteratorType::EMPTY:
                    break;

                case EIteratorType::INPLACE:
                    Entry_ = other.Entry_;
                    EntryConsumed_ = other.EntryConsumed_;
                    break;

                case EIteratorType::LIST:
                    Node_ = other.Node_;
                    break;
                }

                other.BlockIndex_ = nullptr;
            }
            return *this;
        }

        TMaybe<TBlockStorage::TRowEntry> Next() {
            Y_ENSURE(IsValid());

            switch (Type_) {
            case EIteratorType::EMPTY:
                return Nothing();

            case EIteratorType::INPLACE:
                if (EntryConsumed_) {
                    return Nothing();
                }

                EntryConsumed_ = true;
                return BlockIndex_->IsKeyEquals(Entry_, ItemsToLookup_) ? TMaybe<TBlockStorage::TRowEntry>(Entry_) : Nothing();

            case EIteratorType::LIST:
                for (; Node_ != nullptr; Node_ = Node_->Next) {
                    if (BlockIndex_->IsKeyEquals(Node_->Entry, ItemsToLookup_)) {
                        auto entry = Node_->Entry;
                        Node_ = Node_->Next;
                        return entry;
                    }
                }

                return Nothing();
            }
        }

        bool IsValid() const {
            return BlockIndex_;
        }

        bool IsEmpty() const {
            Y_ENSURE(IsValid());

            switch (Type_) {
            case EIteratorType::EMPTY:
                return true;
            case EIteratorType::INPLACE:
                return EntryConsumed_;
            case EIteratorType::LIST:
                return Node_ == nullptr;
            }
        }

        void Reset() {
            *this = TIterator();
        }

    private:
        TIterator(const TBlockIndex* blockIndex)
            : Type_(EIteratorType::EMPTY)
            , BlockIndex_(blockIndex)
        {}

        TIterator(const TBlockIndex* blockIndex, TBlockStorage::TRowEntry entry, std::vector<NYql::NUdf::TBlockItem> itemsToLookup)
            : Type_(EIteratorType::INPLACE)
            , BlockIndex_(blockIndex)
            , Entry_(entry)
            , EntryConsumed_(false)
            , ItemsToLookup_(std::move(itemsToLookup))
        {}

        TIterator(const TBlockIndex* blockIndex, TIndexNode* node, std::vector<NYql::NUdf::TBlockItem> itemsToLookup)
            : Type_(EIteratorType::LIST)
            , BlockIndex_(blockIndex)
            , Node_(node)
            , ItemsToLookup_(std::move(itemsToLookup))
        {}

    private:
        EIteratorType Type_;
        const TBlockIndex* BlockIndex_ = nullptr;

        union {
            TIndexNode* Node_;
            struct {
                TBlockStorage::TRowEntry Entry_;
                bool EntryConsumed_;
            };
        };

        std::vector<NYql::NUdf::TBlockItem> ItemsToLookup_;
    };

public:
    TBlockIndex(
        TMemoryUsageInfo* memInfo,
        const TVector<ui32>& keyColumns,
        NUdf::TUnboxedValue blockStorage,
        bool any,
        TStringBuf resourceTag
    )
        : TBase(memInfo)
        , KeyColumns_(keyColumns)
        , BlockStorage_(std::move(blockStorage))
        , Any_(any)
        , ResourceTag_(std::move(resourceTag))
    {}

    void BuildIndex() {
        if (Index_) {
            return;
        }

        auto& blockStorage = *static_cast<TBlockStorage*>(BlockStorage_.GetResource());
        Y_ENSURE(blockStorage.IsFinished(), "Index build should be done after all data has been read");

        Index_ = std::make_unique<TIndexMap>(CalculateRHHashTableCapacity(blockStorage.GetRowCount()));
        for (size_t blockOffset = 0; blockOffset < blockStorage.GetBlockCount(); blockOffset++) {
            const auto& block = blockStorage.GetBlock(blockOffset);
            auto blockSize = block.Size;

            std::array<TRobinHoodBatchRequestItem<ui64>, PrefetchBatchSize> insertBatch;
            std::array<TBlockStorage::TRowEntry, PrefetchBatchSize> insertBatchEntries;
            std::array<std::vector<NYql::NUdf::TBlockItem>, PrefetchBatchSize> insertBatchKeys;
            ui32 insertBatchLen = 0;

            auto processInsertBatch = [&]() {
                Index_->BatchInsert({insertBatch.data(), insertBatchLen}, [&](size_t i, TIndexMap::iterator iter, bool isNew) {
                    auto value = static_cast<TIndexMapValue*>(Index_->GetMutablePayload(iter));
                    if (isNew) {
                        // Store single entry inplace
                        *value = TIndexMapValue(insertBatchEntries[i]);
                        Index_->CheckGrow();
                    } else {
                        if (Any_ && ContainsKey(value, insertBatchKeys[i])) {
                            return;
                        }

                        // Store as list
                        if (value->IsInplace()) {
                            *value = TIndexMapValue(InsertIndexNode(value->GetEntry()));
                        }

                        *value = TIndexMapValue(InsertIndexNode(insertBatchEntries[i], value->GetList()));
                    }
                });
            };

            Y_ENSURE(blockOffset <= std::numeric_limits<ui32>::max());
            Y_ENSURE(blockSize <= std::numeric_limits<ui32>::max());
            for (size_t itemOffset = 0; itemOffset < blockSize; itemOffset++) {
                auto keyHash = GetKey(block, itemOffset, insertBatchKeys[insertBatchLen]);
                if (!keyHash) {
                    continue;
                }

                insertBatchEntries[insertBatchLen] = TBlockStorage::TRowEntry(blockOffset, itemOffset);
                insertBatch[insertBatchLen].ConstructKey(*keyHash);
                insertBatchLen++;

                if (insertBatchLen == PrefetchBatchSize) {
                    processInsertBatch();
                    insertBatchLen = 0;
                }
            }

            if (insertBatchLen > 0) {
                processInsertBatch();
            }
        }
    }

    template<typename TGetKey>
    void BatchLookup(size_t batchSize, std::array<TIterator, PrefetchBatchSize>& iterators, TGetKey&& getKey) {
        Y_ENSURE(batchSize <= PrefetchBatchSize);

        std::array<TRobinHoodBatchRequestItem<ui64>, PrefetchBatchSize> lookupBatch;
        std::array<std::vector<NYql::NUdf::TBlockItem>, PrefetchBatchSize> itemsBatch;
        std::array<bool, PrefetchBatchSize> notNullBatch = {};

        for (size_t i = 0; i < batchSize; i++) {
            const auto& [items, keyHash] = getKey(i);
            if (!keyHash) {
                lookupBatch[i].ConstructKey(0);
                continue;
            }

            notNullBatch[i] = true;
            lookupBatch[i].ConstructKey(*keyHash);
            itemsBatch[i] = items;
        }

        Index_->BatchLookup({lookupBatch.data(), batchSize}, [&](size_t i, TIndexMap::iterator iter) {
            if (!notNullBatch[i] || !iter) {
                // Empty iterator
                iterators[i] = TIterator(this);
                return;
            }

            auto value = static_cast<const TIndexMapValue*>(Index_->GetPayload(iter));
            if (value->IsInplace()) {
                iterators[i] = TIterator(this, value->GetEntry(), std::move(itemsBatch[i]));
            } else {
                iterators[i] = TIterator(this, value->GetList(), std::move(itemsBatch[i]));
            }
        });
    }

    bool IsKeyEquals(TBlockStorage::TRowEntry entry, const std::vector<NYql::NUdf::TBlockItem>& keyItems) const {
        auto& blockStorage = *static_cast<TBlockStorage*>(BlockStorage_.GetResource());

        Y_ENSURE(keyItems.size() == KeyColumns_.size());
        for (size_t i = 0; i < KeyColumns_.size(); i++) {
            auto indexItem = blockStorage.GetItem(entry, KeyColumns_[i]);
            if (blockStorage.GetItemComparators()[KeyColumns_[i]]->Equals(indexItem, keyItems[i])) {
                return true;
            }
        }

        return false;
    }

    const NUdf::TUnboxedValue& GetBlockStorage() const {
        return BlockStorage_;
    }

private:
    TMaybe<ui64> GetKey(const TBlockStorage::TBlock& block, size_t offset, std::vector<NYql::NUdf::TBlockItem>& keyItems) const {
        auto& blockStorage = *static_cast<TBlockStorage*>(BlockStorage_.GetResource());

        ui64 keyHash = 0;
        keyItems.clear();
        for (ui32 keyColumn : KeyColumns_) {
            auto item = blockStorage.GetItemFromBlock(block, keyColumn, offset);
            if (!item) {
                keyItems.clear();
                return {};
            }

            keyHash = CombineHashes(keyHash, blockStorage.GetItemHashers()[keyColumn]->Hash(item));
            keyItems.push_back(std::move(item));
        }

        return keyHash;
    }

    TIndexNode* InsertIndexNode(TBlockStorage::TRowEntry entry, TIndexNode* currentHead = nullptr) {
        return &IndexNodes_.emplace_back(entry, currentHead);
    }

    bool ContainsKey(const TIndexMapValue* chain, const std::vector<NYql::NUdf::TBlockItem>& keyItems) const {
        if (chain->IsInplace()) {
            return IsKeyEquals(chain->GetEntry(), keyItems);
        } else {
            for (TIndexNode* node = chain->GetList(); node != nullptr; node = node->Next) {
                if (IsKeyEquals(node->Entry, keyItems)) {
                    return true;
                }

                node = node->Next;
            }

            return false;
        }
    }

    NUdf::TStringRef GetResourceTag() const override {
        return NUdf::TStringRef(ResourceTag_);
    }

    void* GetResource() override {
        return this;
    }

private:
    const TVector<ui32>& KeyColumns_;
    NUdf::TUnboxedValue BlockStorage_;

    std::unique_ptr<TIndexMap> Index_;
    std::deque<TIndexNode> IndexNodes_;

    const bool Any_;
    const TStringBuf ResourceTag_;
};

class TBlockMapJoinIndexWrapper : public TMutableComputationNode<TBlockMapJoinIndexWrapper> {
    using TBaseComputation = TMutableComputationNode<TBlockMapJoinIndexWrapper>;

public:
    TBlockMapJoinIndexWrapper(
        TComputationMutables& mutables,
        TVector<ui32>&& keyColumns,
        IComputationNode* blockStorage,
        bool any,
        const TStringBuf& resourceTag
    )
        : TBaseComputation(mutables, EValueRepresentation::Boxed)
        , KeyColumns_(std::move(keyColumns))
        , BlockStorage_(blockStorage)
        , Any_(any)
        , ResourceTag_(resourceTag)
    {}

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        return ctx.HolderFactory.Create<TBlockIndex>(
            KeyColumns_,
            std::move(BlockStorage_->GetValue(ctx)),
            Any_,
            ResourceTag_
        );
    }

private:
    void RegisterDependencies() const final {
        DependsOn(BlockStorage_);
    }

private:
    const TVector<ui32> KeyColumns_;
    IComputationNode* const BlockStorage_;
    const bool Any_;
    const TString ResourceTag_;
};

template <bool WithoutRight, bool RightRequired>
class TBlockMapJoinCoreWraper : public TMutableComputationNode<TBlockMapJoinCoreWraper<WithoutRight, RightRequired>>
{
    using TBaseComputation = TMutableComputationNode<TBlockMapJoinCoreWraper<WithoutRight, RightRequired>>;
    using TJoinState = TBlockJoinState<RightRequired>;
    using TStorageState = TBlockStorage;
    using TIndexState = TBlockIndex;

public:
    TBlockMapJoinCoreWraper(
        TComputationMutables& mutables,
        const TVector<TType*>&& resultItemTypes,
        const TVector<TType*>&& leftItemTypes,
        const TVector<ui32>&& leftKeyColumns,
        const TVector<ui32>&& leftIOMap,
        const TVector<ui32>&& rightIOMap,
        IComputationNode* leftStream,
        IComputationNode* rightBlockIndex
    )
        : TBaseComputation(mutables, EValueRepresentation::Boxed)
        , ResultItemTypes_(std::move(resultItemTypes))
        , LeftItemTypes_(std::move(leftItemTypes))
        , LeftKeyColumns_(std::move(leftKeyColumns))
        , LeftIOMap_(std::move(leftIOMap))
        , RightIOMap_(std::move(rightIOMap))
        , LeftStream_(leftStream)
        , RightBlockIndex_(rightBlockIndex)
        , KeyTupleCache_(mutables)
    {}

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        const auto joinState = ctx.HolderFactory.Create<TJoinState>(
            ctx,
            LeftItemTypes_,
            LeftIOMap_,
            ResultItemTypes_
        );

        return ctx.HolderFactory.Create<TStreamValue>(ctx.HolderFactory,
                                                      std::move(joinState),
                                                      LeftKeyColumns_,
                                                      RightIOMap_,
                                                      std::move(LeftStream_->GetValue(ctx)),
                                                      std::move(RightBlockIndex_->GetValue(ctx))
        );
    }

private:
    class TStreamValue : public TComputationValue<TStreamValue> {
        using TBase = TComputationValue<TStreamValue>;

    public:
        TStreamValue(
            TMemoryUsageInfo* memInfo,
            const THolderFactory& holderFactory,
            NUdf::TUnboxedValue&& joinState,
            const TVector<ui32>& leftKeyColumns,
            const TVector<ui32>& rightIOMap,
            NUdf::TUnboxedValue&& leftStream,
            NUdf::TUnboxedValue&& rightBlockIndex
        )
            : TBase(memInfo)
            , JoinState_(joinState)
            , LeftKeyColumns_(leftKeyColumns)
            , RightIOMap_(rightIOMap)
            , LeftStream_(leftStream)
            , RightBlockIndex_(rightBlockIndex)
            , HolderFactory_(holderFactory)
        {}

    private:
        NUdf::EFetchStatus WideFetch(NUdf::TUnboxedValue* output, ui32 width) {
            auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());
            auto& indexState = *static_cast<TIndexState*>(RightBlockIndex_.GetResource());
            auto& storageState = *static_cast<TStorageState*>(indexState.GetBlockStorage().GetResource());

            if (!RightInputConsumed_) {
                while (storageState.FetchNextBlock()) {
                    // Fetch entire data from the right input
                }

                indexState.BuildIndex();
                RightInputConsumed_ = true;
            }

            auto* inputFields = joinState.GetRawInputFields();
            const size_t inputWidth = joinState.GetInputWidth();
            const size_t outputWidth = joinState.GetOutputWidth();

            MKQL_ENSURE(width == outputWidth,
                        "The given width doesn't equal to the result type size");

            std::vector<NYql::NUdf::TBlockItem> leftKeyColumns(LeftKeyColumns_.size());
            std::vector<ui64> leftKeyColumnHashes(LeftKeyColumns_.size());
            std::vector<NYql::NUdf::TBlockItem> rightRow(RightIOMap_.size());

            while (!joinState.HasBlocks()) {
                while (joinState.IsNotFull() && LookupBatchCurrent_ < LookupBatchSize_) {
                    auto& iter = LookupBatchIterators_[LookupBatchCurrent_];
                    if constexpr (WithoutRight) {
                        if (bool(iter.IsEmpty()) != RightRequired) {
                            joinState.CopyRow();
                        }

                        joinState.NextRow();
                        LookupBatchCurrent_++;
                        continue;
                    } else if constexpr (!RightRequired) {
                        if (iter.IsEmpty()) {
                            joinState.MakeRow(std::vector<NYql::NUdf::TBlockItem>());
                            joinState.NextRow();
                            LookupBatchCurrent_++;
                            continue;
                        }
                    }

                    while (joinState.IsNotFull() && !iter.IsEmpty()) {
                        auto key = iter.Next();
                        storageState.GetRow(*key, RightIOMap_, rightRow);
                        joinState.MakeRow(rightRow);
                    }

                    if (iter.IsEmpty()) {
                        joinState.NextRow();
                        LookupBatchCurrent_++;
                    }
                }

                if (joinState.IsNotFull() && joinState.RemainingRowsCount() > 0) {
                    LookupBatchSize_ = std::min(PrefetchBatchSize, static_cast<ui32>(joinState.RemainingRowsCount()));
                    indexState.BatchLookup(LookupBatchSize_, LookupBatchIterators_, [&](size_t i) {
                        MakeLeftKeys(leftKeyColumns, leftKeyColumnHashes, i);
                        auto keyHash = CalculateTupleHash(leftKeyColumns, leftKeyColumnHashes);
                        return std::make_pair(std::ref(leftKeyColumns), keyHash);
                    });

                    LookupBatchCurrent_ = 0;
                    continue;
                }

                if (joinState.IsNotFull() && !joinState.IsFinished()) {
                    switch (LeftStream_.WideFetch(inputFields, inputWidth)) {
                    case NUdf::EFetchStatus::Yield:
                        return NUdf::EFetchStatus::Yield;
                    case NUdf::EFetchStatus::Ok:
                        joinState.Reset();
                        continue;
                    case NUdf::EFetchStatus::Finish:
                        joinState.Finish();
                        break;
                    }
                    // Leave the loop, if no values left in the stream.
                    Y_DEBUG_ABORT_UNLESS(joinState.IsFinished());
                }
                if (joinState.IsEmpty()) {
                    return NUdf::EFetchStatus::Finish;
                }
                joinState.MakeBlocks(HolderFactory_);
            }

            const auto sliceSize = joinState.Slice();

            for (size_t i = 0; i < outputWidth; i++) {
                output[i] = joinState.Get(sliceSize, HolderFactory_, i);
            }

            return NUdf::EFetchStatus::Ok;
        }

        void MakeLeftKeys(std::vector<NYql::NUdf::TBlockItem>& items, std::vector<ui64>& hashes, size_t offset) const {
            auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());

            Y_ENSURE(items.size() == LeftKeyColumns_.size());
            Y_ENSURE(hashes.size() == LeftKeyColumns_.size());
            for (size_t i = 0; i < LeftKeyColumns_.size(); i++) {
                std::tie(items[i], hashes[i]) = joinState.GetItemWithHash(LeftKeyColumns_[i], offset);
            }
        }

        NUdf::TUnboxedValue JoinState_;

        const TVector<ui32>& LeftKeyColumns_;

        const TVector<ui32>& RightIOMap_;
        bool RightInputConsumed_ = false;

        std::array<typename TIndexState::TIterator, PrefetchBatchSize> LookupBatchIterators_;
        ui32 LookupBatchCurrent_ = 0;
        ui32 LookupBatchSize_ = 0;

        NUdf::TUnboxedValue LeftStream_;
        NUdf::TUnboxedValue RightBlockIndex_;

        const THolderFactory& HolderFactory_;
    };

    void RegisterDependencies() const final {
        this->DependsOn(LeftStream_);
        this->DependsOn(RightBlockIndex_);
    }

private:
    const TVector<TType*> ResultItemTypes_;

    const TVector<TType*> LeftItemTypes_;
    const TVector<ui32> LeftKeyColumns_;
    const TVector<ui32> LeftIOMap_;

    const TVector<ui32> RightIOMap_;

    IComputationNode* const LeftStream_;
    IComputationNode* const RightBlockIndex_;

    const TContainerCacheOnContext KeyTupleCache_;
};

class TBlockCrossJoinCoreWraper : public TMutableComputationNode<TBlockCrossJoinCoreWraper>
{
    using TBaseComputation = TMutableComputationNode<TBlockCrossJoinCoreWraper>;
    using TJoinState = TBlockJoinState<true>;
    using TStorageState = TBlockStorage;

public:
    TBlockCrossJoinCoreWraper(
        TComputationMutables& mutables,
        const TVector<TType*>&& resultItemTypes,
        const TVector<TType*>&& leftItemTypes,
        const TVector<ui32>&& leftIOMap,
        const TVector<ui32>&& rightIOMap,
        IComputationNode* leftStream,
        IComputationNode* rightBlockStorage
    )
        : TBaseComputation(mutables, EValueRepresentation::Boxed)
        , ResultItemTypes_(std::move(resultItemTypes))
        , LeftItemTypes_(std::move(leftItemTypes))
        , LeftIOMap_(std::move(leftIOMap))
        , RightIOMap_(std::move(rightIOMap))
        , LeftStream_(std::move(leftStream))
        , RightBlockStorage_(std::move(rightBlockStorage))
        , KeyTupleCache_(mutables)
    {}

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        const auto joinState = ctx.HolderFactory.Create<TJoinState>(
            ctx,
            LeftItemTypes_,
            LeftIOMap_,
            ResultItemTypes_
        );

        return ctx.HolderFactory.Create<TStreamValue>(ctx.HolderFactory,
                                                      std::move(joinState),
                                                      RightIOMap_,
                                                      std::move(LeftStream_->GetValue(ctx)),
                                                      std::move(RightBlockStorage_->GetValue(ctx))
        );
    }

private:
    class TStreamValue : public TComputationValue<TStreamValue> {
        using TBase = TComputationValue<TStreamValue>;

    public:
        TStreamValue(
            TMemoryUsageInfo* memInfo,
            const THolderFactory& holderFactory,
            NUdf::TUnboxedValue&& joinState,
            const TVector<ui32>& rightIOMap,
            NUdf::TUnboxedValue&& leftStream,
            NUdf::TUnboxedValue&& rightBlockStorage
        )
            : TBase(memInfo)
            , JoinState_(joinState)
            , RightIOMap_(rightIOMap)
            , LeftStream_(leftStream)
            , RightBlockStorage_(rightBlockStorage)
            , HolderFactory_(holderFactory)
        {}

    private:
        NUdf::EFetchStatus WideFetch(NUdf::TUnboxedValue* output, ui32 width) {
            auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());
            auto& storageState = *static_cast<TStorageState*>(RightBlockStorage_.GetResource());

            if (!RightInputConsumed_) {
                while (storageState.FetchNextBlock()) {
                    // Fetch entire data from the right input
                }

                RightInputConsumed_ = true;
                RightRowIterator_ = storageState.GetRowIterator();
            }

            auto* inputFields = joinState.GetRawInputFields();
            const size_t inputWidth = joinState.GetInputWidth();
            const size_t outputWidth = joinState.GetOutputWidth();

            MKQL_ENSURE(width == outputWidth,
                        "The given width doesn't equal to the result type size");

            std::vector<NYql::NUdf::TBlockItem> rightRow(RightIOMap_.size());
            while (!joinState.HasBlocks()) {
                while (!RightRowIterator_.IsEmpty() && joinState.RemainingRowsCount() > 0 && joinState.IsNotFull()) {
                    auto rowEntry = *RightRowIterator_.Next();
                    storageState.GetRow(rowEntry, RightIOMap_, rightRow);
                    joinState.MakeRow(rightRow);
                }

                if (joinState.IsNotFull() && joinState.RemainingRowsCount() > 0) {
                    joinState.NextRow();
                    RightRowIterator_ = storageState.GetRowIterator();
                    continue;
                }

                if (joinState.IsNotFull() && !joinState.IsFinished()) {
                    switch (LeftStream_.WideFetch(inputFields, inputWidth)) {
                    case NUdf::EFetchStatus::Yield:
                        return NUdf::EFetchStatus::Yield;
                    case NUdf::EFetchStatus::Ok:
                        joinState.Reset();
                        continue;
                    case NUdf::EFetchStatus::Finish:
                        joinState.Finish();
                        break;
                    }
                    // Leave the loop, if no values left in the stream.
                    Y_DEBUG_ABORT_UNLESS(joinState.IsFinished());
                }
                if (joinState.IsEmpty()) {
                    return NUdf::EFetchStatus::Finish;
                }
                joinState.MakeBlocks(HolderFactory_);
            }

            const auto sliceSize = joinState.Slice();

            for (size_t i = 0; i < outputWidth; i++) {
                output[i] = joinState.Get(sliceSize, HolderFactory_, i);
            }

            return NUdf::EFetchStatus::Ok;
        }

        NUdf::TUnboxedValue JoinState_;

        const TVector<ui32>& RightIOMap_;
        bool RightInputConsumed_ = false;

        TStorageState::TRowIterator RightRowIterator_;

        NUdf::TUnboxedValue LeftStream_;
        NUdf::TUnboxedValue RightBlockStorage_;

        const THolderFactory& HolderFactory_;
    };

    void RegisterDependencies() const final {
        this->DependsOn(LeftStream_);
        this->DependsOn(RightBlockStorage_);
    }

private:
    const TVector<TType*> ResultItemTypes_;

    const TVector<TType*> LeftItemTypes_;
    const TVector<ui32> LeftIOMap_;

    const TVector<ui32> RightIOMap_;

    IComputationNode* const LeftStream_;
    IComputationNode* const RightBlockStorage_;

    const TContainerCacheOnContext KeyTupleCache_;
};

} // namespace

IComputationNode* WrapBlockStorage(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 1, "Expected 1 arg");

    const auto resultType = callable.GetType()->GetReturnType();
    MKQL_ENSURE(resultType->IsResource(), "Expected Resource as a result type");
    auto resultResourceType = AS_TYPE(TResourceType, resultType);
    MKQL_ENSURE(resultResourceType->GetTag().StartsWith(BlockStorageResourcePrefix), "Expected block storage resource");

    const auto inputType = callable.GetInput(0).GetStaticType();
    MKQL_ENSURE(inputType->IsList(), "Expected List as an input stream");
    const auto inputItemType = AS_TYPE(TListType, inputType)->GetItemType();;
    MKQL_ENSURE(inputItemType->IsStruct(), "Expected Struct as a list item type");

    const auto list = LocateNode(ctx.NodeLocator, callable, 0);
    return new TBlockStorageWrapper(
        ctx.Mutables,
        AS_TYPE(TStructType, inputItemType),
        list,
        resultResourceType->GetTag()
    );
}

IComputationNode* WrapBlockMapJoinIndex(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 4, "Expected 4 args");

    const auto resultType = callable.GetType()->GetReturnType();
    MKQL_ENSURE(resultType->IsResource(), "Expected Resource as a result type");
    auto resultResourceType = AS_TYPE(TResourceType, resultType);
    MKQL_ENSURE(resultResourceType->GetTag().StartsWith(BlockMapJoinIndexResourcePrefix), "Expected block map join index resource");

    const auto inputType = callable.GetInput(0).GetStaticType();
    MKQL_ENSURE(inputType->IsResource(), "Expected Resource as an input type");
    auto inputResourceType = AS_TYPE(TResourceType, inputType);
    MKQL_ENSURE(inputResourceType->GetTag().StartsWith(BlockStorageResourcePrefix), "Expected block storage resource");

    auto origInputItemType = AS_VALUE(TTypeType, callable.GetInput(1));
    MKQL_ENSURE(origInputItemType->IsStruct(), "Expected Struct as an input item type");
    const auto origInputItemStructType = AS_TYPE(TStructType, origInputItemType);
    MKQL_ENSURE(origInputItemStructType->GetMembersCount() > 0, "Expected at least one column");

    const auto keyColumnsLiteral = callable.GetInput(2);
    const auto keyColumnsTuple = AS_VALUE(TTupleLiteral, keyColumnsLiteral);
    TVector<ui32> keyColumns;
    keyColumns.reserve(keyColumnsTuple->GetValuesCount());
    for (ui32 i = 0; i < keyColumnsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, keyColumnsTuple->GetValue(i));
        keyColumns.emplace_back(item->AsValue().Get<ui32>());
    }

    for (ui32 keyColumn : keyColumns) {
        MKQL_ENSURE(keyColumn < origInputItemStructType->GetMembersCount(), "Key column out of range");
    }

    const auto anyNode = callable.GetInput(3);
    const auto any = AS_VALUE(TDataLiteral, anyNode)->AsValue().Get<bool>();

    const auto blockStorage = LocateNode(ctx.NodeLocator, callable, 0);
    return new TBlockMapJoinIndexWrapper(
        ctx.Mutables,
        std::move(keyColumns),
        blockStorage,
        any,
        resultResourceType->GetTag()
    );
}

IComputationNode* WrapBlockMapJoinCore(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 8, "Expected 8 args");

    const auto joinType = callable.GetType()->GetReturnType();
    MKQL_ENSURE(joinType->IsStream(), "Expected WideStream as a resulting stream");
    const auto joinStreamType = AS_TYPE(TStreamType, joinType);
    MKQL_ENSURE(joinStreamType->GetItemType()->IsMulti(),
                "Expected Multi as a resulting item type");
    const auto joinComponents = GetWideComponents(joinStreamType);
    MKQL_ENSURE(joinComponents.size() > 0, "Expected at least one column");
    const TVector<TType*> joinItems(joinComponents.cbegin(), joinComponents.cend());

    const auto leftType = callable.GetInput(0).GetStaticType();
    MKQL_ENSURE(leftType->IsStream(), "Expected WideStream as a left stream");
    const auto leftStreamType = AS_TYPE(TStreamType, leftType);
    MKQL_ENSURE(leftStreamType->GetItemType()->IsMulti(),
                "Expected Multi as a left stream item type");
    const auto leftStreamComponents = GetWideComponents(leftStreamType);
    MKQL_ENSURE(leftStreamComponents.size() > 0, "Expected at least one column");
    const TVector<TType*> leftStreamItems(leftStreamComponents.cbegin(), leftStreamComponents.cend());

    const auto joinKindNode = callable.GetInput(3);
    const auto rawKind = AS_VALUE(TDataLiteral, joinKindNode)->AsValue().Get<ui32>();
    const auto joinKind = GetJoinKind(rawKind);
    Y_ENSURE(joinKind == EJoinKind::Inner || joinKind == EJoinKind::Left ||
             joinKind == EJoinKind::LeftSemi || joinKind == EJoinKind::LeftOnly || joinKind == EJoinKind::Cross);

    const auto rightBlockStorageType = callable.GetInput(1).GetStaticType();
    MKQL_ENSURE(rightBlockStorageType->IsResource(), "Expected Resource as a right type");
    auto rightBlockStorageResourceType = AS_TYPE(TResourceType, rightBlockStorageType);
    if (joinKind != EJoinKind::Cross) {
        MKQL_ENSURE(rightBlockStorageResourceType->GetTag().StartsWith(BlockMapJoinIndexResourcePrefix), "Expected block map join index resource");
    } else {
        MKQL_ENSURE(rightBlockStorageResourceType->GetTag().StartsWith(BlockStorageResourcePrefix), "Expected block storage resource");
    }

    auto origRightItemType = AS_VALUE(TTypeType, callable.GetInput(2));
    MKQL_ENSURE(origRightItemType->IsStruct(), "Expected Struct as a right stream item type");
    const auto origRightItemStructType = AS_TYPE(TStructType, origRightItemType);
    MKQL_ENSURE(origRightItemStructType->GetMembersCount() > 0, "Expected at least one column");

    const auto leftKeyColumnsLiteral = callable.GetInput(4);
    const auto leftKeyColumnsTuple = AS_VALUE(TTupleLiteral, leftKeyColumnsLiteral);
    TVector<ui32> leftKeyColumns;
    leftKeyColumns.reserve(leftKeyColumnsTuple->GetValuesCount());
    for (ui32 i = 0; i < leftKeyColumnsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, leftKeyColumnsTuple->GetValue(i));
        leftKeyColumns.emplace_back(item->AsValue().Get<ui32>());
    }
    const THashSet<ui32> leftKeySet(leftKeyColumns.cbegin(), leftKeyColumns.cend());

    const auto leftKeyDropsLiteral = callable.GetInput(5);
    const auto leftKeyDropsTuple = AS_VALUE(TTupleLiteral, leftKeyDropsLiteral);
    THashSet<ui32> leftKeyDrops;
    leftKeyDrops.reserve(leftKeyDropsTuple->GetValuesCount());
    for (ui32 i = 0; i < leftKeyDropsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, leftKeyDropsTuple->GetValue(i));
        leftKeyDrops.emplace(item->AsValue().Get<ui32>());
    }

    for (const auto& drop : leftKeyDrops) {
        MKQL_ENSURE(leftKeySet.contains(drop),
                    "Only key columns has to be specified in drop column set");
    }

    const auto rightKeyColumnsLiteral = callable.GetInput(6);
    const auto rightKeyColumnsTuple = AS_VALUE(TTupleLiteral, rightKeyColumnsLiteral);
    TVector<ui32> rightKeyColumns;
    rightKeyColumns.reserve(rightKeyColumnsTuple->GetValuesCount());
    for (ui32 i = 0; i < rightKeyColumnsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, rightKeyColumnsTuple->GetValue(i));
        rightKeyColumns.emplace_back(item->AsValue().Get<ui32>());
    }
    const THashSet<ui32> rightKeySet(rightKeyColumns.cbegin(), rightKeyColumns.cend());

    const auto rightKeyDropsLiteral = callable.GetInput(7);
    const auto rightKeyDropsTuple = AS_VALUE(TTupleLiteral, rightKeyDropsLiteral);
    THashSet<ui32> rightKeyDrops;
    rightKeyDrops.reserve(rightKeyDropsTuple->GetValuesCount());
    for (ui32 i = 0; i < rightKeyDropsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, rightKeyDropsTuple->GetValue(i));
        rightKeyDrops.emplace(item->AsValue().Get<ui32>());
    }

    for (const auto& drop : rightKeyDrops) {
        MKQL_ENSURE(rightKeySet.contains(drop),
                    "Only key columns has to be specified in drop column set");
    }

    if (joinKind == EJoinKind::Cross) {
        MKQL_ENSURE(leftKeyColumns.empty() && leftKeyDrops.empty() && rightKeyColumns.empty() && rightKeyDrops.empty(),
                    "Specifying key columns is not allowed for cross join");
    }
    MKQL_ENSURE(leftKeyColumns.size() == rightKeyColumns.size(), "Key columns mismatch");

    // XXX: Mind the last wide item, containing block length.
    TVector<ui32> leftIOMap;
    for (size_t i = 0; i < leftStreamItems.size() - 1; i++) {
        if (leftKeyDrops.contains(i)) {
            continue;
        }
        leftIOMap.push_back(i);
    }

    // XXX: Mind the last wide item, containing block length.
    TVector<ui32> rightIOMap;
    if (joinKind == EJoinKind::Inner || joinKind == EJoinKind::Left || joinKind == EJoinKind::Cross) {
        for (size_t i = 0; i < origRightItemStructType->GetMembersCount(); i++) {
            if (rightKeyDrops.contains(i) || origRightItemStructType->GetMemberName(i) == NYql::BlockLengthColumnName) {
                continue;
            }
            rightIOMap.push_back(i);
        }
    } else {
        MKQL_ENSURE(rightKeyDrops.empty(), "Right key drops are not allowed for semi/only join");
    }

    const auto leftStream = LocateNode(ctx.NodeLocator, callable, 0);
    const auto rightBlockStorage = LocateNode(ctx.NodeLocator, callable, 1);

#define JOIN_WRAPPER(WITHOUT_RIGHT, RIGHT_REQUIRED)                                 \
    return new TBlockMapJoinCoreWraper<WITHOUT_RIGHT, RIGHT_REQUIRED>(              \
        ctx.Mutables,                                                               \
        std::move(joinItems),                                                       \
        std::move(leftStreamItems),                                                 \
        std::move(leftKeyColumns),                                                  \
        std::move(leftIOMap),                                                       \
        std::move(rightIOMap),                                                      \
        leftStream,                                                                 \
        rightBlockStorage                                                           \
    )

    switch (joinKind) {
    case EJoinKind::Inner:
        JOIN_WRAPPER(false, true);
    case EJoinKind::Left:
        JOIN_WRAPPER(false, false);
    case EJoinKind::LeftSemi:
        MKQL_ENSURE(rightIOMap.empty(), "Can't access right table on left semi join");
        JOIN_WRAPPER(true, true);
    case EJoinKind::LeftOnly:
        MKQL_ENSURE(rightIOMap.empty(), "Can't access right table on left only join");
        JOIN_WRAPPER(true, false);
    case EJoinKind::Cross:
        return new TBlockCrossJoinCoreWraper(
            ctx.Mutables,
            std::move(joinItems),
            std::move(leftStreamItems),
            std::move(leftIOMap),
            std::move(rightIOMap),
            leftStream,
            rightBlockStorage
        );
    default:
        /* TODO: Display the human-readable join kind name. */
        MKQL_ENSURE(false, "BlockMapJoinCore doesn't support join type #"
                    << static_cast<ui32>(joinKind));
    }

#undef JOIN_WRAPPER
}

} // namespace NMiniKQL
} // namespace NKikimr
