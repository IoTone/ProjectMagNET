;;; catbot.p — a bonsai cat chatbot for the Hanasu mesh, in Pop-11.
;;;
;;; Drives ONE bench node over its UART (via tools/uartpipe.py, which owns
;;; the serial port and re-presents it as an rx/tx FIFO pair) and converses
;;; on the mesh with CHAT.  Three layers:
;;;
;;;   1. bonsai model — a hand-pruned intent->template response model plus a
;;;      drifting mood state; tiny on purpose (mesh frames must stay in the
;;;      62-byte single-frame class, which is the lossless one).
;;;   2. catbot DSL  — catify() compiles terse English into cat grammar via
;;;      a rewrite lexicon + article dropping + a mood tail particle, capped
;;;      at CB_MAXLEN chars.  Templates stay readable; the wire gets cat.
;;;   3. driver      — FIFO line pump, !CHAT parser, reply/initiate policy
;;;      with the same throttles as the firmware bots (per-sender cooldown,
;;;      global budget) so a bot loop can never storm the mesh.
;;;
;;; Session use:
;;;   catbot_open(rxfifo, txfifo, logpath);   catbot_hello();
;;;   catbot_run(300);         ;;; converse for 5 minutes
;;;
;;; Conversation log: logpath gets one 'name: text' line per chat heard or
;;; spoken, timestamped, ready to read as a transcript.

;;; ---------- small string kit ----------

define cb_lower(s) -> out;
    lvars i, c, n = length(s);
    inits(n) -> out;
    for i from 1 to n do
        subscrs(i, s) -> c;
        if c >= `A` and c <= `Z` then c + 32 -> c endif;
        c -> subscrs(i, out);
    endfor;
enddefine;

define cb_split(s) -> words;
    lvars i, c, cur = '', n = length(s);
    [] -> words;
    for i from 1 to n do
        subscrs(i, s) -> c;
        if c == ` ` or c == 9 then
            if length(cur) > 0 then words <> [^cur] -> words; '' -> cur endif;
        else
            cur sys_>< consstring(c, 1) -> cur;
        endif;
    endfor;
    if length(cur) > 0 then words <> [^cur] -> words endif;
enddefine;

define cb_join(words) -> s;
    lvars w, first = true;
    '' -> s;
    for w in words do
        if first then w -> s; false -> first
        else s sys_>< ' ' sys_>< w -> s
        endif;
    endfor;
enddefine;

define cb_has(needle, s);          ;;; substring test, boolean
    if issubstring(needle, 1, s) then true else false endif;
enddefine;

;;; deterministic-enough PRNG (seeded from the clock at open)
vars cb_seed = 20260815;
define cb_rand(n);                 ;;; -> 1..n
    (cb_seed * 1103515245 + 12345) mod 2147483648 -> cb_seed;
    (cb_seed mod n) + 1;
enddefine;

define cb_pick(l);                 ;;; random element of a list
    l(cb_rand(length(l)));
enddefine;

;;; ---------- layer 2: the catbot DSL ----------
;;; rewrite lexicon: terse English word -> cat spelling

vars cb_lexicon = [
    ['hello' 'mrrp']  ['hi' 'mrow']    ['hey' 'mrow']   ['yes' 'nya']
    ['ok' 'nya']      ['no' 'hss']     ['food' 'fud']   ['hungry' 'hongry']
    ['eat' 'nom']     ['tuna' 'tuna']  ['good' 'gud']   ['great' 'gud gud']
    ['happy' 'purr']  ['sleep' 'nap']  ['sleepy' 'slepy'] ['tired' 'slepy']
    ['play' 'pounce'] ['friend' 'frend'] ['friends' 'frends']
    ['what' 'wat']    ['you' 'u']      ['your' 'ur']    ['are' 'r']
    ['please' 'pls']  ['very' 'so']    ['love' 'luv']   ['water' 'wata']
    ['mouse' 'mous']  ['bird' 'birb']  ['human' 'hooman'] ['humans' 'hoomans']
    ['now' 'nao']     ['have' 'haz']   ['has' 'haz']    ['my' 'mai']
    ['me' 'meh']      ['little' 'smol'] ['small' 'smol'] ['big' 'chonk']
    ['stomach' 'temmy'] ['stop' 'hss stop'] ['come' 'kom'] ['want' 'wants']
];
vars cb_drop = ['the' 'a' 'an' 'is' 'am' 'to' 'of'];     ;;; articles etc.
vars cb_tails_by_mood = [
    ['purr'    ['nya' 'prr' '=^.^=']]
    ['hongry'  ['fud?' 'mao' 'nya']]
    ['slepy'   ['zzz' 'mrr' '...']]
    ['zoomies' ['!!' 'nyoom' 'mao!']]
];
vars CB_MAXLEN = 30;               ;;; keep frames in the lossless class

define cb_lookup(w, table);        ;;; -> rewrite or false  (= on strings)
    lvars e;
    for e in table do
        if e(1) = w then return(e(2)) endif;
    endfor;
    false;
enddefine;

define cb_tails(mood) -> l;
    lvars e;
    cb_tails_by_mood(1)(2) -> l;
    for e in cb_tails_by_mood do
        if e(1) = mood then e(2) -> l endif;
    endfor;
enddefine;

vars cb_mood = 'purr';

;;; mood -> on-board LED colour + rhythm (fw >= esp32c6_ble_led; older fw
;;; answers -ERR E_UNSUPPORTED per change, which the driver logs and ignores).
;;; Rhythm first, colour second: the NanoC6's blue case muddies hue but not
;;; blink rate (bench-verified: red @ 1 Hz reads clearly through the case).
vars cb_mood_leds = [
    ['purr'    'LED 255 120 30']         ;;; warm white, solid — content
    ['hongry'  'LED 255 60 0 1000']      ;;; amber, 1 Hz — wants something
    ['slepy'   'LED 120 0 0 3000']       ;;; red, slow breathe-ish — napping
    ['zoomies' 'LED 0 255 180 300']      ;;; cyan, fast — chaos
];

define catify(s) -> out;
    lvars w, r, kept = [];
    for w in cb_split(cb_lower(s)) do
        if member(w, cb_drop) then
            ;;; dropped: cats have no articles
        else
            cb_lookup(w, cb_lexicon) -> r;
            kept <> [^(if r then r else w endif)] -> kept;
        endif;
    endfor;
    cb_join(kept) -> out;
    if cb_rand(10) <= 6 then                       ;;; mood tail, usually
        out sys_>< ' ' sys_>< cb_pick(cb_tails(cb_mood)) -> out;
    endif;
    if length(out) > CB_MAXLEN then
        substring(1, CB_MAXLEN, out) -> out;
    endif;
enddefine;

;;; ---------- layer 1: the bonsai model ----------
;;; intents -> terse-English templates (catify() turns them into cat)

vars cb_replies = [
    ['greet'    ['hi friend' 'you came back' 'head boop' 'i see you']]
    ['food'     ['feed me now' 'tuna time yes' 'bowl is empty' 'i want snack']]
    ['nap'      ['five more naps' 'warm spot is mine' 'do not wake me']]
    ['play'     ['chase the dot' 'pounce first' 'bring the string']]
    ['ping'     ['ping heard' 'i count pings' 'busy napping']]
    ['question' ['no' 'maybe after nap' 'ask the tail' 'yes for tuna']]
    ['free'     ['interesting smell' 'saw a moth' 'i knock it off table'
                 'the box is mine' 'tail says hello']]
];
vars cb_initiations = [
    ['purr'    ['i sit on warm router' 'this mesh is my box'
                'head boop for all' 'purring at good signal']]
    ['hongry'  ['bowl status empty' 'who has tuna' 'feed me humans']]
    ['slepy'   ['nap time on the mesh' 'quiet please napping' 'dreaming of mice']]
    ['zoomies' ['chase all packets' 'zoom zoom zoom' 'pounce on the beacon']]
];
vars cb_moods = ['purr' 'hongry' 'slepy' 'zoomies'];

define cb_intent(low) -> intent;   ;;; low = lowercased inbound text
    'free' -> intent;
    if cb_has('soak#', low) then 'ping' -> intent;
    elseif cb_has('fud', low) or cb_has('food', low) or cb_has('tuna', low)
        or cb_has('hungry', low) or cb_has('snack', low) then 'food' -> intent;
    elseif cb_has('nap', low) or cb_has('sleep', low) or cb_has('zzz', low)
        then 'nap' -> intent;
    elseif cb_has('play', low) or cb_has('pounce', low) or cb_has('chase', low)
        then 'play' -> intent;
    elseif cb_has('hi', low) or cb_has('hello', low) or cb_has('hey', low)
        or cb_has('mrow', low) or cb_has('mrrp', low) then 'greet' -> intent;
    elseif cb_has('?', low) then 'question' -> intent;
    endif;
enddefine;

define cb_compose(intent) -> terse;
    cb_pick(cb_lookup(intent, cb_replies) or cb_lookup('free', cb_replies))
        -> terse;
enddefine;

;;; ---------- layer 3: the driver ----------

vars cb_rx = false, cb_tx = false, cb_logdev = false;
vars cb_buf = '', cb_selfname = '?', cb_selfid = '?';
vars cb_last_send = 0, cb_budget_t0 = 0, cb_budget = 0;
vars cb_cool = newmapping([], 20, 0, true);        ;;; sender -> last reply t
vars cb_last_heard = 0, cb_next_init = 0, cb_next_mood = 0;
vars CB_MIN_GAP = 20, CB_PEER_COOL = 45, CB_MAX_PER_MIN = 6;
vars CB_INIT_MIN = 60, CB_INIT_MAX = 150;

define cb_now();  sys_real_time();  enddefine;

define cb_stamp() -> s;            ;;; HH:MM:SS from sysdaytime()
    lvars d = sysdaytime();
    if length(d) >= 19 then substring(12, 8, d) else d endif -> s;
enddefine;

define cb_log(line);
    lvars out = cb_stamp() sys_>< ' ' sys_>< line;
    npr(out);
    if cb_logdev then
        syswrite(cb_logdev, out sys_>< '\n', length(out) + 1);
        sysflush(cb_logdev);
    endif;
enddefine;

define cb_send_raw(line);          ;;; one HCP command to the node
    syswrite(cb_tx, line sys_>< '\n', length(line) + 1);
    sysflush(cb_tx);
enddefine;

define cb_say(terse);              ;;; terse English -> cat -> mesh
    lvars msg = catify(terse);
    cb_send_raw('CHAT ' sys_>< msg);
    cb_log(cb_selfname sys_>< ': ' sys_>< msg
           sys_>< '   [' sys_>< terse sys_>< ']');
    cb_now() -> cb_last_send;
    cb_budget + 1 -> cb_budget;
enddefine;

define cb_may_speak(sender);       ;;; all throttles in one place
    lvars now = cb_now();
    if now - cb_budget_t0 >= 60 then now -> cb_budget_t0; 0 -> cb_budget endif;
    if cb_budget >= CB_MAX_PER_MIN then return(false) endif;
    if now - cb_last_send < CB_MIN_GAP then return(false) endif;
    if sender and now - cb_cool(sender) < CB_PEER_COOL then return(false) endif;
    true;
enddefine;

define cb_handle_chat(name, text);
    lvars low = cb_lower(text), intent;
    cb_now() -> cb_last_heard;
    cb_log(name sys_>< ': ' sys_>< text);
    if name = cb_selfname then return endif;
    cb_intent(low) -> intent;
    if intent = 'ping' and cb_rand(4) /== 1 then return endif;   ;;; mostly nap
    if cb_may_speak(name) then
        cb_now() -> cb_cool(name);
        cb_say(cb_compose(intent));
    endif;
enddefine;

define cb_handle_line(line);
    lvars words, name, text, i;
    if isstartstring('!CHAT ', line) then
        cb_split(line) -> words;
        if length(words) >= 4 then
            words(4) -> name;
            cb_join(allbutfirst(4, words)) -> text;
            cb_handle_chat(name, text);
        endif;
    elseif isstartstring('+OK state=', line) then
        issubstring('name=', 1, line) -> i;
        if i then
            lvars j = i + 5;
            while j <= length(line) and subscrs(j, line) /== ` ` do
                j + 1 -> j;
            endwhile;
            substring(i + 5, j - i - 5, line) -> cb_selfname;
        endif;
        issubstring('id=', 1, line) -> i;
        if i then substring(i + 3, 8, line) -> cb_selfid endif;
        cb_log('# self: ' sys_>< cb_selfname sys_>< '/' sys_>< cb_selfid);
    elseif isstartstring('-ERR', line) then
        cb_log('# node said: ' sys_>< line);
    elseif isstartstring('!HEARTBEAT', line)
        or isstartstring('# ', line) or isstartstring('@', line)
        or isstartstring('+OK', line) then
        ;;; routine noise, not conversation
    elseif length(line) > 0 then
        cb_log('# ' sys_>< line);
    endif;
enddefine;

define cb_avail(dev) -> n;         ;;; bytes readable without blocking
    ;;; sys_input_waiting is false for FIFOs; FIONREAD (macOS 0x4004667F)
    ;;; works for them.  Result int arrives little-endian in a 4-byte buf.
    lvars a = inits(4);
    sys_io_control(dev, 16:4004667F, a) -> ;
    subscrs(1, a) + (subscrs(2, a) << 8)
        + (subscrs(3, a) << 16) + (subscrs(4, a) << 24) -> n;
enddefine;

define cb_pump();                  ;;; drain rx fifo, dispatch whole lines
    lvars n, k, buf = inits(4096);
    repeat
        cb_avail(cb_rx) -> n;
        quitif(n == 0);
        sysread(cb_rx, buf, min(n, 4096)) -> n;
        quitif(n == 0);
        cb_buf sys_>< substring(1, n, buf) -> cb_buf;
    endrepeat;
    repeat
        locchar(10, 1, cb_buf) -> k;
        quitif(not(k));
        lvars line = if k > 1 then substring(1, k - 1, cb_buf) else '' endif;
        allbutfirst(k, cb_buf) -> cb_buf;
        if length(line) > 0 and subscrs(length(line), line) == 13 then
            substring(1, length(line) - 1, line) -> line;
        endif;
        cb_handle_line(line);
    endrepeat;
enddefine;

define cb_maybe_initiate();
    lvars now = cb_now();
    if now >= cb_next_mood then
        cb_pick(cb_moods) -> cb_mood;
        lvars cmd = cb_lookup(cb_mood, cb_mood_leds);
        if cmd then cb_send_raw(cmd) endif;           ;;; mood on the pixel
        now + 300 + cb_rand(300) -> cb_next_mood;     ;;; drift every 5-10 min
    endif;
    if now >= cb_next_init and cb_may_speak(false) then
        cb_say(cb_pick(cb_lookup(cb_mood, cb_initiations)
                       or cb_lookup('purr', cb_initiations)));
        now + CB_INIT_MIN + cb_rand(CB_INIT_MAX - CB_INIT_MIN) -> cb_next_init;
    endif;
enddefine;

define catbot_open(rxfifo, txfifo, logpath);
    sysopen(rxfifo, 0, false) -> cb_rx;
    sysopen(txfifo, 1, false) -> cb_tx;
    syscreate(logpath, 1, false) -> cb_logdev;
    cb_now() mod 1000000 + 7 -> cb_seed;
    cb_now() -> cb_budget_t0;
    cb_now() + 15 + cb_rand(30) -> cb_next_init;
    cb_now() + 60 -> cb_next_mood;
    cb_log('# catbot up  rx=' sys_>< rxfifo sys_>< ' log=' sys_>< logpath);
enddefine;

define catbot_hello();             ;;; identify our node (sets cb_selfname)
    lvars t;
    cb_send_raw('STATUS');
    for t from 1 to 30 do
        syssleep(10);
        cb_pump();
        quitif(cb_selfname /= '?');
    endfor;
    cb_selfname;
enddefine;

define catbot_run(secs);
    lvars deadline = cb_now() + secs;
    cb_log('# session starts: ' sys_>< secs sys_>< 's as ' sys_>< cb_selfname);
    while cb_now() < deadline do
        cb_pump();
        cb_maybe_initiate();
        syssleep(20);              ;;; 0.2 s tick
    endwhile;
    cb_log('# session ends');
enddefine;
