\ Role: spy-snapper — periodic camera capture through the H5 lifecycle.
\ The host owns the timer: role-tick fires every tick_ms (envelope field);
\ this bundle never loops or sleeps, which is what makes it hot-swappable.
\ caps_req: ["camera"]   (M5_Hive_Camera advertises this)

: snap        cam-snap drop ;
: role-init   snap ;          \ one confirmation capture at install
: role-tick   snap ;          \ periodic capture, host-owned cadence
: role-status ." spy-snapper ticking" cr ;
