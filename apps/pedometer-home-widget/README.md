# Pedometer Home Widget

A home screen widget for Maemo Leste on the F(x)tec Pro1X: steps, distance
and calories for today and for the week, the last seven days or all time,
and a daily goal. Tapping it shows the last seven days as a bar chart.

It is a port of Andrei Mirestean's Pedometer Home Widget for Maemo 5
(pedometerhomewidget 0.3-4, GPL-3.0-or-later), with the same settings
(unit, walking or running, height class or own step length, weight) and the
same formulas for distance and calories.

The N900 widget counted the steps itself and read the accelerometer every
25 ms to do so. On the Pro1X the sensor core counts them in hardware, and
`pro1x-steps` (tools/pro1x-steps) offers the count on the system bus. The
widget only listens while it is on the visible desktop (pro1x-steps sends
at most one signal every five seconds); otherwise it reads the count when it
comes back into view and once just before midnight, so it does not wake the
phone while it is in the pocket.

Build and install: `make install` (scripts/49-apps.sh does it). Data:
`~/.config/pedometer-home-widget/` (settings.ini, history, state).
