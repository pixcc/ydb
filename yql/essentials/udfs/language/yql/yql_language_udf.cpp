#include <yql/essentials/public/udf/udf_helpers.h>

#include <yql/essentials/sql/v1/lexer/antlr4/lexer.h>
#include <yql/essentials/sql/v1/lexer/antlr4_ansi/lexer.h>
#include <yql/essentials/sql/v1/proto_parser/proto_parser.h>
#include <yql/essentials/sql/v1/proto_parser/antlr4/proto_parser.h>
#include <yql/essentials/sql/v1/proto_parser/antlr4_ansi/proto_parser.h>
#include <yql/essentials/sql/v1/format/sql_format.h>
#include <yql/essentials/sql/settings/translation_settings.h>
#include <library/cpp/protobuf/util/simple_reflection.h>

using namespace NYql;
using namespace NKikimr::NUdf;
using namespace NSQLTranslation;

class TRuleFreqVisitor {
public:
    TRuleFreqVisitor() {
    }

    void Visit(const NProtoBuf::Message& msg) {
        const NProtoBuf::Descriptor* descr = msg.GetDescriptor();
        if (descr->name() == "TToken") {
            return;
        }

        TStringBuf fullName = descr->full_name();
        fullName.SkipPrefix("NSQLv1Generated.");
        for (int i = 0; i < descr->field_count(); ++i) {
            const NProtoBuf::FieldDescriptor* fd = descr->field(i);
            NProtoBuf::TConstField field(msg, fd);
            if (!field.HasValue()) {
                continue;
            }

            TStringBuf fieldFullName = fd->full_name();
            fieldFullName.SkipPrefix("NSQLv1Generated.");
            if (fieldFullName.EndsWith(".Descr")) {
                continue;
            }


            Freqs[std::make_pair(fullName, fieldFullName)] += 1;
        }

        VisitAllFields(msg, descr);
    }

    const THashMap<std::pair<TString, TString>, ui64>& GetFreqs() const {
        return Freqs;
    }

private:
    void VisitAllFields(const NProtoBuf::Message& msg, const NProtoBuf::Descriptor* descr) {
        for (int i = 0; i < descr->field_count(); ++i) {
            const NProtoBuf::FieldDescriptor* fd = descr->field(i);
            NProtoBuf::TConstField field(msg, fd);
            if (field.IsMessage()) {
                for (size_t j = 0; j < field.Size(); ++j) {
                    Visit(*field.Get<NProtoBuf::Message>(j));
                }
            }
        }
    }

    THashMap<std::pair<TString, TString>, ui64> Freqs;
};

SIMPLE_UDF(TObfuscate, TOptional<char*>(TAutoMap<char*>)) {
    using namespace NSQLFormat;
    try {
        const auto sqlRef = args[0].AsStringRef();
        TString formattedQuery;
        NYql::TIssues issues;
        google::protobuf::Arena arena;
        NSQLTranslation::TTranslationSettings settings;
        settings.Arena = &arena;
        NSQLTranslationV1::TLexers lexers;
        lexers.Antlr4 = NSQLTranslationV1::MakeAntlr4LexerFactory();
        lexers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiLexerFactory();
        NSQLTranslationV1::TParsers parsers;
        parsers.Antlr4 = NSQLTranslationV1::MakeAntlr4ParserFactory();
        parsers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiParserFactory();
        if (!MakeSqlFormatter(lexers, parsers, settings)->Format(TString(sqlRef), formattedQuery, issues, EFormatMode::Obfuscate)) {
            return {};
        }

        return valueBuilder->NewString(formattedQuery);
    } catch (const yexception&) {
        return {};
    }
}

using TRuleFreqResult = TListType<TTuple<char*, char*, ui64>>;

SIMPLE_UDF(TRuleFreq, TOptional<TRuleFreqResult>(TAutoMap<char*>)) {
    try {
        const TString query(args[0].AsStringRef());
        NYql::TIssues issues;
        google::protobuf::Arena arena;
        NSQLTranslation::TTranslationSettings settings;
        settings.Arena = &arena;
        if (!ParseTranslationSettings(query, settings, issues)) {
            return {};
        }

        NSQLTranslationV1::TLexers lexers;
        lexers.Antlr4 = NSQLTranslationV1::MakeAntlr4LexerFactory();
        lexers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiLexerFactory();
        auto lexer = NSQLTranslationV1::MakeLexer(lexers, settings.AnsiLexer, true);
        auto onNextToken = [&](NSQLTranslation::TParsedToken&& token) {
            Y_UNUSED(token);
        };

        if (!lexer->Tokenize(query, "", onNextToken, issues, NSQLTranslation::SQL_MAX_PARSER_ERRORS)) {
            return {};
        }

        NSQLTranslationV1::TParsers parsers;
        parsers.Antlr4 = NSQLTranslationV1::MakeAntlr4ParserFactory();
        parsers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiParserFactory();
        auto msg = NSQLTranslationV1::SqlAST(parsers, query, "", issues, NSQLTranslation::SQL_MAX_PARSER_ERRORS,
            settings.AnsiLexer, true, &arena);
        if (!msg) {
            return {};
        }

        TRuleFreqVisitor visitor;
        visitor.Visit(*msg);

        auto listBuilder = valueBuilder->NewListBuilder();
        for (const auto& [key, f] : visitor.GetFreqs()) {
            TUnboxedValue* items;
            auto tuple = valueBuilder->NewArray(3, items);
            items[0] = valueBuilder->NewString(key.first);
            items[1] = valueBuilder->NewString(key.second);
            items[2] = TUnboxedValuePod(f);
            listBuilder->Add(std::move(tuple));
        }

        return listBuilder->Build();
    } catch (const yexception&) {
        return {};
    }
}

SIMPLE_MODULE(TYqlLangModule,
    TObfuscate,
    TRuleFreq
);

REGISTER_MODULES(TYqlLangModule);
