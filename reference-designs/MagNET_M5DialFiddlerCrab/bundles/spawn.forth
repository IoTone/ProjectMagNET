\ Role: spawn — minimal "I joined the hive" identity bundle.
\ Installs a single greeting word and announces itself once at install time.
\ caps_req: [] (works on any node)

: hello-spawn ." Hello from the spawn role" cr ;
hello-spawn
