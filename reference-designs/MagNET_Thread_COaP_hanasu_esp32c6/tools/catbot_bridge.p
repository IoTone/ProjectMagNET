;;; catbot_bridge.p — nekobot transport v2: drive the node THROUGH the
;;; chat-bench bridge (tools/chat-bench/bridge.py) instead of owning serial.
;;;
;;; Load AFTER catbot.p; it redefines exactly two procedures — cb_send_raw
;;; and cb_pump — so the whole bonsai/DSL/throttle stack runs unchanged over
;;; HTTP/SSE. The bridge is the sole serial owner (port collisions become
;;; structurally impossible), and its single clock gives every utterance a
;;; measured per-receiver latency, which this layer accounts like a mini
;;; soak: delivery %, latency list, misses.
;;;
;;; Event flow: an external `curl -sN /events | jq …` pipeline (started by
;;; the session driver) appends TSV lines to a file; cb_pump tails it.
;;; Line shapes:  chat <ts> <node> <sender> <text…>
;;;               info <ts> <node> <name>
;;;               photo <ts> <node> <from> <fname> <size> <secs>
;;;               xferfail <ts> <node> <reason…>
;;;
;;; Session use:
;;;   load tools/catbot.p          load tools/catbot_bridge.p
;;;   catbot2_open('<events.tsv>', 'neko', '<logpath>');
;;;   catbot_run(21600);           ;;; unchanged main loop
;;;   cb2_report();                ;;; delivery/latency summary into the log

vars cb2_url = 'http://127.0.0.1:8642';
vars cb2_ev = false, cb2_evbuf = '';
vars cb2_self = -1, cb2_nnodes = 0;
vars cb2_pending = [];                 ;;; {text t0 seenlist} vectors, oldest first
vars cb2_lats = [];                    ;;; delivered latencies, ms
vars cb2_del = 0, cb2_exp = 0, cb2_missct = 0;
vars cb2_photos = 0, cb2_xferfails = 0, cb2_cmderrs = 0;
vars CB2_AGE_OUT = 30;

;;; ---- tiny JSON/HTTP kit (payloads are ours: no quotes to escape) ----

define cb2_post(path, body) -> reply;
    lvars r = sys_obey_linerep('set +e; curl -s -m 25 -X POST '
        sys_>< cb2_url sys_>< path sys_>< ' -d \'' sys_>< body sys_>< '\'');
    lvars l;
    '' -> reply;
    repeat
        r() -> l;
        quitif(l == termin);
        reply sys_>< l -> reply;
    endrepeat;
enddefine;

define cb2_json_num(key, s) -> v;      ;;; first "key": <number> in s
    lvars pat = '"' sys_>< key sys_>< '":', i, j, st;
    false -> v;
    issubstring(pat, 1, s) -> i;
    if i then
        i + length(pat) -> j;
        while j <= length(s) and subscrs(j, s) == ` ` do j + 1 -> j endwhile;
        j -> st;
        while j <= length(s)
              and (isnumbercode(subscrs(j, s)) or subscrs(j, s) == `.`) do
            j + 1 -> j;
        endwhile;
        if j > st then strnumber(substring(st, j - st, s)) -> v endif;
    endif;
enddefine;

define cb2_split_tab(line, nfields);
    ;;; leaves ( fields rest ) on the stack: first nfields tab-separated
    ;;; fields as a list, remainder as one string. Collect with
    ;;;     cb2_split_tab(l, n) -> rest -> fields;
    lvars i = 1, k, got = [];
    repeat
        quitif(length(got) == nfields);
        locchar(9, i, line) -> k;
        quitif(not(k));
        got <> [^(substring(i, k - i, line))] -> got;
        k + 1 -> i;
    endrepeat;
    got;
    if i <= length(line) then substring(i, length(line) - i + 1, line)
    else ''
    endif;
enddefine;

define cb2_get(path) -> reply;
    lvars r = sys_obey_linerep('set +e; curl -s -m 10 ' sys_>< cb2_url sys_>< path);
    lvars l;
    '' -> reply;
    repeat
        r() -> l;
        quitif(l == termin);
        reply sys_>< l -> reply;
    endrepeat;
enddefine;

;;; ---- TRANSPORT OVERRIDE 1: sending ----
;;; CHAT goes via POST /send (bridge stamps t0 for latency); every other
;;; HCP line (LED, STATS, …) goes via POST /cmd.

define cb_send_raw(line);
    lvars rep;
    if isstartstring('CHAT ', line) then
        cb2_post('/send', '{"node":' sys_>< cb2_self
                 sys_>< ',"text":"' sys_>< allbutfirst(5, line) sys_>< '"}')
            -> rep;
        lvars t0 = cb2_json_num('ts', rep);
        if issubstring('"ok": true', 1, rep) and t0 then
            cb2_pending <> [^(consvector(allbutfirst(5, line), t0, [], 3))]
                -> cb2_pending;
        else
            cb2_cmderrs + 1 -> cb2_cmderrs;
            cb_log('# send failed: ' sys_>< rep);
        endif;
    else
        cb2_post('/cmd', '{"node":' sys_>< cb2_self
                 sys_>< ',"line":"' sys_>< line sys_>< '"}') -> rep;
        unless issubstring('"ok": true', 1, rep) then
            cb2_cmderrs + 1 -> cb2_cmderrs;
            cb_log('# cmd failed: ' sys_>< line sys_>< ' -> ' sys_>< rep);
        endunless;
    endif;
enddefine;

;;; ---- TRANSPORT OVERRIDE 2: receiving ----

define cb2_note_delivery(node, ts, text);
    lvars v, seen, lat;
    for v in cb2_pending do
        if v(1) = text and not(member(node, v(3))) then
            v(3) <> [^node] -> v(3);
            if ts and v(2) then
                round((ts - v(2)) * 1000) -> lat;
                cb2_lats <> [^lat] -> cb2_lats;
            endif;
            return;
        endif;
    endfor;
enddefine;

define cb2_handle_ev(line);
    lvars fields, rest, kind, ts, node;
    cb2_split_tab(line, 3) -> rest -> fields;
    if length(fields) < 3 then return endif;
    fields(1) -> kind;
    strnumber(fields(2)) -> ts;
    strnumber(fields(3)) -> node;
    if kind = 'chat' then
        lvars f2, sender, text;
        cb2_split_tab(rest, 1) -> text -> f2;
        if length(f2) < 1 then return endif;
        f2(1) -> sender;
        if node == cb2_self then
            cb_handle_chat(sender, text);          ;;; the model replies here
        else
            cb2_note_delivery(node, ts, text);
        endif;
    elseif kind = 'photo' then
        cb2_photos + 1 -> cb2_photos;
        cb_log('# photo landed on node ' sys_>< node sys_>< ': ' sys_>< rest);
    elseif kind = 'xferfail' then
        cb2_xferfails + 1 -> cb2_xferfails;
        cb_log('# xfer FAIL on node ' sys_>< node sys_>< ': ' sys_>< rest);
    endif;
    ;;; info lines are consumed during catbot2_open; quiet afterwards
enddefine;

define cb2_age_out();
    lvars now = cb_now(), v, keep = [];
    for v in cb2_pending do
        if now - v(2) < CB2_AGE_OUT then
            keep <> [^v] -> keep;
        else
            cb2_del + length(v(3)) -> cb2_del;
            cb2_exp + (cb2_nnodes - 1) -> cb2_exp;
            if length(v(3)) < cb2_nnodes - 1 then
                cb2_missct + 1 -> cb2_missct;
                cb_log('# MISS "' sys_>< v(1) sys_>< '" seen by only '
                       sys_>< length(v(3)) sys_>< '/' sys_>< cb2_nnodes - 1);
            endif;
        endif;
    endfor;
    keep -> cb2_pending;
enddefine;

define cb_pump();
    lvars buf = inits(8192), n, k;
    repeat
        sysread(cb2_ev, buf, 8192) -> n;
        quitif(n == 0);
        cb2_evbuf sys_>< substring(1, n, buf) -> cb2_evbuf;
    endrepeat;
    repeat
        locchar(10, 1, cb2_evbuf) -> k;
        quitif(not(k));
        lvars line = if k > 1 then substring(1, k - 1, cb2_evbuf) else '' endif;
        allbutfirst(k, cb2_evbuf) -> cb2_evbuf;
        if length(line) > 0 then cb2_handle_ev(line) endif;
    endrepeat;
    cb2_age_out();
enddefine;

;;; ---- session bring-up ----

define catbot2_open(evpath, selfname, logpath);
    syscreate(logpath, 1, false) -> cb_logdev;
    selfname -> cb_selfname;
    cb_now() mod 1000000 + 7 -> cb_seed;
    cb_now() -> cb_budget_t0;
    cb_now() + 15 + cb_rand(30) -> cb_next_init;
    cb_now() + 60 -> cb_next_mood;
    ;;; how many panes does the bridge hold?
    lvars rep = cb2_get('/nodes');
    cb2_json_num('count', rep) -> cb2_nnodes;
    unless cb2_nnodes then mishap('bridge unreachable', [^rep]) endunless;
    ;;; find our node index from the info snapshots in the event stream
    sysopen(evpath, 0, false) -> cb2_ev;
    lvars t, fields, rest;
    for t from 1 to 300 do                 ;;; snapshots arrive every ~5 s
        lvars buf = inits(8192), n, k;
        repeat
            sysread(cb2_ev, buf, 8192) -> n;
            quitif(n == 0);
            cb2_evbuf sys_>< substring(1, n, buf) -> cb2_evbuf;
        endrepeat;
        repeat
            locchar(10, 1, cb2_evbuf) -> k;
            quitif(not(k));
            lvars line = substring(1, k - 1, cb2_evbuf);
            allbutfirst(k, cb2_evbuf) -> cb2_evbuf;
            cb2_split_tab(line, 3) -> rest -> fields;
            if length(fields) = 3 and fields(1) = 'info'
                    and rest = selfname then
                strnumber(fields(3)) -> cb2_self;
            endif;
        endrepeat;
        quitif(cb2_self >= 0);
        syssleep(10);
    endfor;
    if cb2_self < 0 then mishap('self node not seen in event stream', [^selfname]) endif;
    cb_log('# catbot v2 up: bridge=' sys_>< cb2_url
           sys_>< ' self=' sys_>< selfname sys_>< '@' sys_>< cb2_self
           sys_>< ' of ' sys_>< cb2_nnodes sys_>< ' nodes  events=' sys_>< evpath);
enddefine;

define cb2_report();
    lvars pct = if cb2_exp > 0 then 100.0 * cb2_del / cb2_exp else 0 endif;
    lvars n = length(cb2_lats), s = 0, mx = 0, l;
    for l in cb2_lats do s + l -> s; max(mx, l) -> mx endfor;
    cb_log('# v2 REPORT: delivery ' sys_>< cb2_del sys_>< '/' sys_>< cb2_exp
           sys_>< ' (' sys_>< pct sys_>< '%)  misses=' sys_>< cb2_missct);
    if n > 0 then
        cb_log('# v2 REPORT: latency n=' sys_>< n
               sys_>< ' avg=' sys_>< round(s / n) sys_>< 'ms max=' sys_>< mx sys_>< 'ms');
    endif;
    cb_log('# v2 REPORT: photos=' sys_>< cb2_photos
           sys_>< ' xferfails=' sys_>< cb2_xferfails
           sys_>< ' cmderrs=' sys_>< cb2_cmderrs);
enddefine;
