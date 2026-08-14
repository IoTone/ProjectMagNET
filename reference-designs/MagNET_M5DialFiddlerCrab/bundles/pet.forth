\ Role 8: Pet — the ruler's cute animal. Barks on command; barking at
\ strangers arrives with hive peer-event FFI (future).
\ Host: any Capsule Scribe (buzz + hive words).
\ caps_req: ["kv-store"]  tick_ms: 1500
: bark        700 90 buzz  500 120 buzz ;
: role-init   bark  s" tame" s" pet:state" hkv-put$ ;
: role-tick   s" pet:cmd" hkv-run ;
: role-status ." pet: woof (pet:cmd)" cr ;
