# The module is powered on VSYS through D2; 3V3_OUT stays unconnected

+5 V from the cart reaches the module only as `VSYS_PICO` through Schottky D2, and the module's 3V3_OUT pin is NC. All peripheral logic runs from the board's own AMS1117 +3V3 rail. This keeps the module's buck-boost from being back-fed or loaded by the buffers, at the cost of a second 3.3 V regulator on the board.
