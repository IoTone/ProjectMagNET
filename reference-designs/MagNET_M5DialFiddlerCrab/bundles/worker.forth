\ Role 2: Worker — carries out tasks the Ruler leaves in worker:task.
\ Tasks are Forth phrases; hkv-run fetches, clears the key, then evaluates
\ (at-most-once). Any node with the hive words can be a worker.
\ caps_req: []  tick_ms: 2000
: role-init   s" idle" s" worker:state" hkv-put$ ;
: role-tick   s" worker:task" hkv-run ;
: role-status ." worker polling worker:task" cr ;
