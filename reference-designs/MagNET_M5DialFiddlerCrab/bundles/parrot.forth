\ Role 3: Parrot — echoes whatever appears in parrot:say to parrot:echo.
\ v0.1 re-echoes each tick until the ruler clears parrot:say (no string
\ compare word in the stub core yet to detect "already echoed").
\ caps_req: []  tick_ms: 2000
: role-tick
    s" parrot:say" hkv-get$ if
      s" parrot:echo" hkv-put$
    then ;
: role-status ." parrot listening on parrot:say" cr ;
