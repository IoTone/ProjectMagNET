\ Role 12: Boombox — composed sounds on top of the firmware's tone
\ primitives, plus a Forth command channel (boombox:forth) distinct from
\ the firmware's C-side boombox:cmd poller.
\ Host: MagNET_ReSpeaker_Boombox.
\ caps_req: ["tone"]  tick_ms: 1000
: dit    880 90 tone  60 sleep ;
: dah    880 240 tone  60 sleep ;
: sos    dit dit dit  dah dah dah  dit dit dit ;
: role-init   notify ;
: role-tick   s" boombox:forth" hkv-run ;
: role-status ." boombox ready (sos via boombox:forth)" cr ;
