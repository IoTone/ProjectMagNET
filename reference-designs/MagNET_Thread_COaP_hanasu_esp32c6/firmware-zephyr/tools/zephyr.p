;;; zephyr.p — Pop-11 wrappers for the MagNET Zephyr/MG24 port.
;;;
;;;   popsession start --name mg24
;;;   popsession send --name mg24 -f firmware-zephyr/tools/zephyr.p
;;;
;;;   zb('mn');               build ~/zephyrproject/build/mn, print summary
;;;   zb_new('mn', app, extra) pristine build of app dir (extra: cmake args or '')
;;;   zb_sys('mnota', app, extra) sysbuild (MCUboot + signed app)
;;;   zfl('mn');              flash via the Silabs Arduino OpenOCD
;;;   zram('mn', 15);         top-N static RAM symbols (data+bss)
;;;   zcrash('/path/log');    resolve a Zephyr fault dump's pc/lr to source
;;;   zinv('/path/file.c');   ESP-IDF / FreeRTOS / NVS API used by a file
;;;
;;; Every helper shells out once and parses in-session; the value is the
;;; parsing (dedupe, ranking, resolution) staying compiled between calls.

vars zp_top = '$HOME/zephyrproject';
vars zp_env = 'set +e; cd $HOME/zephyrproject && . .venv/bin/activate && '
    sys_>< 'export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1 && ';
vars zp_oo = '$HOME/Library/Arduino15/packages/SiliconLabs/tools/openocd/0.12.0-arduino1-static';
vars zp_bin = '$HOME/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-';

define zp_lines(cmd) -> l;
    lvars r = sys_obey_linerep(cmd), x;
    [% repeat r() -> x; quitif(x == termin); x endrepeat %] -> l;
enddefine;

define zp_has(line, subs);
    lvars s;
    for s in subs do if issubstring(s, 1, line) then return(true) endif endfor;
    false
enddefine;

;;; strip everything up to and including the last '/' of any absolute path
;;; prefix, so errors read "magnet_core.c:252:9: error: ..."
define zp_short(line) -> line;
    lvars i = issubstring(': error', 1, line) or issubstring(': warning', 1, line)
              or issubstring(': undefined', 1, line), j;
    returnunless(i);
    i -> j;
    while j > 1 and subscrs(j - 1, line) /== `/` and subscrs(j - 1, line) /== ` ` do
        j - 1 -> j
    endwhile;
    substring(j, datalength(line) - j + 1, line) -> line;
enddefine;

;;; Summarise build output: unique errors/warnings (with repeat counts),
;;; then the memory-region lines. Returns true iff the build succeeded.
define zp_summary(lines) -> ok;
    lvars l, seen = newmapping([], 64, 0, true), order = [], sizes = [], k;
    true -> ok;
    for l in lines do
        if zp_has(l, ['error:' 'undefined reference' 'overflowed' 'FAILED:' 'Error:' 'FATAL ERROR']) then
            false -> ok
        endif;
        if zp_has(l, ['error:' 'warning:' 'undefined reference' 'overflowed' 'Error:' 'FATAL ERROR'])
        and not(issubstring('Kconfig', 1, l)) then
            zp_short(l) -> k;
            if seen(k) == 0 then [^^order ^k] -> order endif;
            seen(k) + 1 -> seen(k);
        elseif zp_has(l, ['FLASH:' 'RAM:']) and issubstring('%', 1, l) then
            [^^sizes ^l] -> sizes
        endif;
    endfor;
    for k in order do
        npr(if seen(k) > 1 then '  (x' sys_>< seen(k) sys_>< ') ' else '  ' endif sys_>< k)
    endfor;
    for l in sizes do npr(l) endfor;
    npr(if ok then '== build OK' else '== build FAILED (' sys_>< length(order) sys_>< ' distinct)' endif);
enddefine;

define zb(dir);
    zp_summary(zp_lines(zp_env sys_>< 'west build -d build/' sys_>< dir sys_>< ' 2>&1'))
enddefine;

define zb_new(dir, app, extra);
    zp_summary(zp_lines(zp_env sys_>< 'west build -p always -b xiao_mg24 ' sys_>< app
        sys_>< ' -d build/' sys_>< dir
        sys_>< (if extra = '' then '' else ' -- ' sys_>< extra endif) sys_>< ' 2>&1'))
enddefine;

define zb_sys(dir, app, extra);
    ;;; --sysbuild: MCUboot + signed app (Z-F). Summary covers both images.
    zp_summary(zp_lines(zp_env sys_>< 'west build --sysbuild -p always -b xiao_mg24 ' sys_>< app
        sys_>< ' -d build/' sys_>< dir
        sys_>< (if extra = '' then '' else ' -- ' sys_>< extra endif) sys_>< ' 2>&1'))
enddefine;

define zfl(dir);
    lvars l, ok = false;
    for l in zp_lines(zp_env sys_>< 'west flash -d build/' sys_>< dir
            sys_>< ' -r openocd --openocd ' sys_>< zp_oo sys_>< '/bin/openocd'
            sys_>< ' --openocd-search ' sys_>< zp_oo sys_>< '/share/openocd/scripts 2>&1') do
        if issubstring('wrote ', 1, l) then npr(l); true -> ok
        elseif zp_has(l, ['Error' 'FATAL']) then npr(l)
        endif
    endfor;
    npr(if ok then '== flashed' else '== FLASH FAILED' endif);
    ok
enddefine;

define zp_hex(s) -> n;
    lvars i, c;
    0 -> n;
    for i from 1 to datalength(s) do
        subscrs(i, s) -> c;
        n * 16 + (if c >= `a` then c - `a` + 10 elseif c >= `A` then c - `A` + 10
                  else c - `0` endif) -> n
    endfor;
enddefine;

;;; top-N data/bss symbols from nm -S (sizes are hex)
define zram(dir, n);
    lvars l, parts, rows = [], total = 0, i = 0, sz, r;
    for l in zp_lines('set +e; ' sys_>< zp_bin sys_>< 'nm -S -C ' sys_>< zp_top
            sys_>< '/build/' sys_>< dir sys_>< '/zephyr/zephyr.elf') do
        [% sys_parse_string(l) %] -> parts;
        if length(parts) >= 4 and member(parts(3), ['b' 'B' 'd' 'D']) then
            zp_hex(parts(2)) -> sz;
            total + sz -> total;
            [^^rows [^sz ^(parts(4))]] -> rows
        endif
    endfor;
    syssort(rows, procedure(a, b); a(1) > b(1) endprocedure) -> rows;
    npr('static data+bss symbols: ' sys_>< total sys_>< ' B');
    for r in rows do
        i + 1 -> i; quitif(i > n);
        npr('  ' sys_>< r(1) sys_>< '  ' sys_>< r(2))
    endfor;
enddefine;

;;; pull every 0x080xxxxx address after 'pc' / 'lr:' in a fault dump and
;;; resolve them in one addr2line call
define zcrash_elf(logpath, dir);
    lvars l, addrs = '', i;
    for l in zp_lines('set +e; grep -oE "(r15/pc\\): |lr: )0x[0-9a-f]+" ' sys_>< logpath) do
        issubstring('0x', 1, l) -> i;
        addrs sys_>< ' ' sys_>< substring(i, datalength(l) - i + 1, l) -> addrs
    endfor;
    if addrs = '' then npr('no pc/lr addresses in ' sys_>< logpath); return endif;
    for l in zp_lines('set +e; ' sys_>< zp_bin sys_>< 'addr2line -f -i -C -p -e ' sys_>< zp_top
            sys_>< '/build/' sys_>< dir sys_>< '/zephyr/zephyr.elf' sys_>< addrs
            sys_>< ' | sed -E "s#/Users/[^ ]*/(zephyrproject|firmware-[a-z]+|components)/#\\1/#g"') do
        npr('  ' sys_>< l)
    endfor;
enddefine;

define zcrash(logpath);
    zcrash_elf(logpath, 'mn')
enddefine;

;;; OTA package verifier (magnet_pkg.c) vs tools/mnpkg.py, every spec code
define zpkgtest();
    lvars l;
    for l in zp_lines('set +e; cd $HOME/dev/projects/iotone/ProjectMagNET/reference-designs/'
            sys_>< 'MagNET_Thread_COaP_hanasu_esp32c6 && $HOME/zephyrproject/.venv/bin/python '
            sys_>< 'tools/pkgtest/run.py 2>&1') do
        if issubstring('MISMATCH', 1, l) or issubstring('cases match', 1, l)
        or issubstring('rror', 1, l) then npr(l) endif
    endfor;
enddefine;

;;; ESP/FreeRTOS/NVS API surface of a source file, ranked by use count
define zinv(path);
    lvars l, parts;
    for l in zp_lines('set +e; grep -ohE "\\b(esp_[a-z_0-9]+|heap_caps_[a-z_]+|nvs_[a-z_]+|'
            sys_>< 'x[A-Z][A-Za-z]+|v[A-Z][a-z][A-Za-z]+|pd[A-Z][A-Za-z_]*|port[A-Z_]+|'
            sys_>< 'cJSON_[A-Za-z]+|gpio_[a-z_]+|ESP_[A-Z_]+|mbedtls_[a-z0-9_]+|psa_[a-z_]+)\\b" '
            sys_>< path sys_>< ' | sort | uniq -c | sort -rn') do
        npr(l)
    endfor;
enddefine;

npr('zephyr.p loaded: zb zb_new zb_sys zfl zpkgtest zram zcrash zcrash_elf zinv');
