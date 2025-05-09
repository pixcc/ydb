# Generated by devtools/yamaker from nixpkgs 24.05.

LIBRARY()

LICENSE(
    BSD-2-Clause AND
    BSD-2-Clause-Views AND
    BSD-3-Clause AND
    MIT
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(2025-02-25)

ORIGINAL_SOURCE(https://github.com/libcxxrt/libcxxrt/archive/a6f71cbc3a1e1b8b9df241e081fa0ffdcde96249.tar.gz)

PEERDIR(
    contrib/libs/libunwind
    library/cpp/sanitizer/include
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

NO_UTIL()

CXXFLAGS(-nostdinc++)

IF (SANITIZER_TYPE == undefined OR FUZZING)
    NO_SANITIZE()
    NO_SANITIZE_COVERAGE()
ENDIF()

SRCS(
    auxhelper.cc
    dynamic_cast.cc
    exception.cc
    guard.cc
    memory.cc
    stdexcept.cc
    typeinfo.cc
)

END()
