;;; bench.p — Pop-11 library for the MagNET Hanasu bench.
;;;
;;; The repetitive work of this project is: talk to four nodes over serial,
;;; flash them, and summarise noisy logs. Doing that with a throwaway Python
;;; script each time is slow to write and impossible to reuse; here the helpers
;;; are compiled once into a live session and each later call costs ~ms.
;;;
;;;     popsession start
;;;     popsession send -f tools/bench.p
;;;     popsession send -c 'bench_status();'
;;;
;;; Serial I/O itself lives in tools/hcp.py (pyserial); this drives it.

vars HCP = '~/dev/projects/iotone/ProjectMagNET/reference-designs/MagNET_Thread_COaP_hanasu_esp32c6/tools/hcp.py';
;;; pyserial lives in the PlatformIO venv, not the system python
vars PY  = '~/.platformio/penv/bin/python';
vars PIO = '~/.platformio/penv/bin/pio';
vars FW  = '~/dev/projects/iotone/ProjectMagNET/reference-designs/MagNET_Thread_COaP_hanasu_esp32c6/firmware-idf';

;;; ---------- shell plumbing ----------

define shell_lines(cmd) -> out;
    ;;; Run a command, collect stdout as a list of strings.
    lvars rep = sys_obey_linerep(cmd), line;
    [] -> out;
    repeat
        rep() -> line;
        quitif(line == termin);
        [^^out ^line] -> out;
    endrepeat;
enddefine;

define contains(needle, line);
    issubstring(needle, 1, line) and true
enddefine;

define pick(needle, lines) -> out;
    ;;; every line containing needle
    lvars l;
    [] -> out;
    for l in lines do
        if contains(needle, l) then [^^out ^l] -> out endif;
    endfor;
enddefine;

;;; ---------- node control ----------

define node_ports() -> out;
    shell_lines(PY >< ' ' >< HCP >< ' list') -> out;
enddefine;

define node_cmd(port, cmd) -> out;
    shell_lines(PY >< ' ' >< HCP >< ' ' >< port >< ' ' >< cmd) -> out;
enddefine;

define node_reboot(port) -> out;
    shell_lines(PY >< ' ' >< HCP >< ' ' >< port >< ' --reboot') -> out;
enddefine;

define node_status(port) -> line;
    ;;; the single +OK STATUS line, or false
    lvars ls = pick('+OK state=', node_cmd(port, 'STATUS'));
    if ls == [] then false else hd(ls) endif;
enddefine;

define bench(cmd);
    ;;; Run one HCP command on every attached node, in a single process.
    ;;; The sweep run constantly during bench work — keep it to one spawn.
    lvars l;
    for l in shell_lines(PY >< ' ' >< HCP >< ' all ' >< cmd) do npr(l) endfor;
enddefine;

define bench_status();
    bench('STATUS');
enddefine;

define node_advertising(port);
    ;;; reboot and report whether the BLE provisioning window opened
    pick('provisioning window open', node_reboot(port)) /= []
enddefine;

;;; ---------- flashing ----------

define flash(env, port);
    lvars ls = shell_lines('cd ' >< FW >< ' && ' >< PIO
                >< ' run -e ' >< env >< ' -t upload --upload-port ' >< port
                >< ' 2>&1 | tail -3');
    lvars ok = pick('SUCCESS', ls) /= [];
    npr(env >< ' -> ' >< port >< ': ' >< (if ok then 'SUCCESS' else 'FAILED' endif));
    ok
enddefine;

;;; ---------- log summarising ----------

define strip_tag(line);
    ;;; logcat prefixes "I/flutter (1234): [probe] "; return the tail, or false
    lvars i = issubstring('[probe] ', 1, line);
    if i then substring(i + 8, length(line) - i - 7, line) else false endif
enddefine;

define probe_lines(path) -> out;
    lvars dev = sysopen(path, 0, "line");
    lvars rep = line_repeater(dev, inits(4096)), line, txt;
    [] -> out;
    repeat
        rep() -> line;
        quitif(line == termin);
        strip_tag(line) -> txt;
        if txt then [^^out ^txt] -> out endif;
    endrepeat;
enddefine;

define probe_summary(path);
    lvars ls = probe_lines(path), l, pass = 0, fail = 0;
    for l in ls do
        npr(l);
        if issubstring('PASS ', 1, l) == 1 then pass + 1 -> pass
        elseif issubstring('FAIL ', 1, l) == 1 then fail + 1 -> fail
        endif;
    endfor;
    npr('');
    npr('>>> ' sys_>< pass sys_>< ' pass, ' sys_>< fail sys_>< ' fail');
enddefine;

define probe_failures(path);
    lvars l;
    for l in pick('FAIL ', probe_lines(path)) do npr(l) endfor;
enddefine;

npr('bench.p loaded: bench_status node_cmd node_reboot node_advertising '
    >< 'flash probe_summary probe_failures');
