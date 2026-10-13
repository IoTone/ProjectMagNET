\ Role: hanasu-hello — first signed bundle over the BUNDLE HCP verb (H6).
\ role-init announces itself on the mesh; that is the whole test: install
\ proves Ed25519 verify + rollback engine + persistence on a Thread node.
\ caps_req: []   (any Hanasu node)
: role-init   s" signed bundle v2 online" mn-chat ;
: role-status ." hanasu-hello installed" cr ;
