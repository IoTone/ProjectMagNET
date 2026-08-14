\ Role 9: ML PhD — designs role modifications for ruler/scribe review.
\ The design work happens off-node (sign_bundle.py is its lab bench);
\ on-node it observes and reports. No role-tick: nothing periodic yet.
\ caps_req: []
: role-init   s" observing" s" mlphd:state" hkv-put$ ;
: role-status ." mlphd observing the hive" cr ;
