\ boot.fs — run at every startup, before the prompt appears.
\ Push it with:  pio run -t uploadfs
\ Or write it on the device:  s" boot.fs" edit
\ Hold the BOOT button while powering up to skip this file entirely.

\ Retune the built-in modes.
2000 fast-ms !
8    fast-px !

\ Words defined here are ordinary words; `see`, `forget` and `user` all work.
: wiggle  20 0 do  5 0 move  40 ms  -5 0 move  40 ms  loop ;

\ Pull in more files if you want to split things up.
\ s" hud.fs" include

\ `tick` is what loop() calls. Redefine it and you have replaced the firmware's
\ main loop without reflashing:
\ : tick  jiggler-tick  keepalive ;

\ Start in FAST mode every time.
\ fast mode!
