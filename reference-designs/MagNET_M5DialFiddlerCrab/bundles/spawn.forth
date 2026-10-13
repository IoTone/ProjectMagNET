\ Role: spawn — minimal "I joined the hive" identity bundle.
\ Announces itself once at install via the H5 role-init hook (no top-level
\ execution — install-time behavior belongs in role-init).
\ caps_req: [] (works on any node)

: hello-spawn ." Hello from the spawn role" cr ;
: role-init   hello-spawn ;
: role-status ." spawn idle" cr ;
