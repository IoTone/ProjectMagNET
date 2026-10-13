\ Role 11: Eye — capture on a cadence, tell the hive.
\ Host: M5_Hive_Camera (cam-snap + hkv words).
\ caps_req: ["camera"]  tick_ms: 5000
variable eye-seq
: role-init   0 eye-seq !  cam-snap drop ;
: role-tick
    cam-snap drop
    eye-seq @ 1 + eye-seq !
    s" snap" s" eye:last" hkv-put$ ;
: role-status ." eye seq=" eye-seq @ . cr ;
