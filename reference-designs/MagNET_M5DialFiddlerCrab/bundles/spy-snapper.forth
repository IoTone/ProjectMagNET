\ Role: spy-snapper — single-shot "take one picture" word that an external
\ scheduler can invoke. Real periodic-capture loops should use cooperative
\ Forth tasks once those land; for v1 we keep it as a one-shot so the
\ install doesn't tie up the Forth REPL.
\ caps_req: ["camera"]   (M5_Hive_Camera advertises this)

: snap   cam-snap drop ;

\ Confirm install with one capture (drop the size from the stack).
snap
