# Хостовый компилятор для env:native.
#
# На Windows системного gcc обычно нет, и "pio test -e native" падает
# с "'gcc' is not recognized". При этом MinGW уже лежит в пакетах
# PlatformIO (его тянет за собой сборка под ESP8266), поэтому вместо
# отдельной установки просто добавляем его каталог в PATH сборки.
#
# На Linux и macOS каталога нет, скрипт молча ничего не делает и
# используется системный компилятор.

import os

Import("env")

core_dir = env.subst("$PROJECT_CORE_DIR")
mingw_bin = os.path.join(core_dir, "packages", "toolchain-gccmingw32", "bin")

if os.path.isdir(mingw_bin):
    env.PrependENVPath("PATH", mingw_bin)
    # Собранный тест запускается уже без этого PATH и не находит
    # libstdc++-6.dll / libgcc_s_dw2-1.dll — падает с 0xC0000135
    # ещё до первой проверки. Линкуем рантайм внутрь exe.
    env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])
    print("native: используется MinGW из %s" % mingw_bin)
