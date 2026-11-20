# sourced by import.sh / regen.sh: where Ghidra and Java are, on Windows (MSYS2) or Linux.
# N22TOOLS = the tools folder with ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1 (Windows) and the Ghidra project folder work/
T="${N22TOOLS:?set N22TOOLS to the tools folder (ghidra_12.1.4_PUBLIC, jdk-21.0.12.1+1, work/), e.g. export N22TOOLS=/e/n2xtools}"
WORK=$T/work
GH=$T/ghidra_12.1.4_PUBLIC
mkdir -p "$WORK/scripts"
if command -v cygpath >/dev/null 2>&1; then           # Windows: MSYS2 / MINGW64
    TW="$(cygpath -w "$T")"; WW="$TW\\work"; SEP='\'
    export JAVA_HOME="$TW\\jdk-21.0.12.1+1"
    export PATH="$T/jdk-21.0.12.1+1/bin:$PATH"
    HEADLESS="$GH/support/analyzeHeadless.bat"
    PY=python
else                                                  # Linux: the system's Java 21 (or $JAVA_HOME)
    WW="$WORK"; SEP='/'
    [ -d "$T/jdk-21.0.12.1+1" ] && export JAVA_HOME="$T/jdk-21.0.12.1+1" && export PATH="$JAVA_HOME/bin:$PATH"
    HEADLESS="$GH/support/analyzeHeadless"
    PY=python3
fi
NINJA_OR_MAKE() {   # rebuild when a build folder exists (ninja on Windows, make or ninja on Linux)
    if [ -f "$HERE/build/build.ninja" ]; then ( cd "$HERE" && ninja -C build rg 2>&1 | grep -E 'error|Linking' ) || true
    elif [ -f "$HERE/build/Makefile" ]; then ( cd "$HERE" && make -C build rg -j"${JOBS:-2}" 2>&1 | grep -E 'error|Linking' ) || true; fi
}
