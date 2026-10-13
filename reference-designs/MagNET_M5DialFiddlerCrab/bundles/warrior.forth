\ Role 6: Warrior — stands guard; the Ruler dictates its actions via
\ warrior:order (Forth phrases). Autonomous responses need peer-event FFI
\ (future) — v0.1 executes orders and reports its state.
\ caps_req: []  tick_ms: 2000
: role-init   s" ready" s" warrior:state" hkv-put$ ;
: role-tick   s" warrior:order" hkv-run ;
: role-status ." warrior ready for warrior:order" cr ;
