\ Role 5: Beeper — lights up or makes a noise when asked.
\ Commands are Forth phrases in beeper:cmd, e.g. "1200 100 buzz".
\ Host: any Capsule Scribe (buzz + hive words).
\ caps_req: ["kv-store"]  tick_ms: 1000
: chirp       1500 40 buzz  2200 60 buzz ;
: role-init   chirp ;
: role-tick   s" beeper:cmd" hkv-run ;
: role-status ." beeper armed on beeper:cmd" cr ;
