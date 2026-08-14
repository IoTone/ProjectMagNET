\ Role: scribe-extra — adds a couple of helper words on top of the base
\ scribe-store / scribe-recall vocabulary already registered by the Capsule
\ firmware. Demonstrates that bundles can extend FFI words rather than
\ replace them. Banner moved into the H5 role-init hook.
\ caps_req: ["kv-store"]   (Capsule advertises this)

\ Print a banner whenever the scribe greets the hive.
: scribe-banner
  cr ." === MagNET Scribe ready ===" cr ;

: role-init   scribe-banner ;
: role-status ." scribe-extra loaded" cr ;
