# arduinoClock
This is a quick little project I built at the behest of my 9yo daughter who wanted an LED alarm clock for her room. It's pretty basic. Uses a DS3231 real time clock and a TM1637 LED clock display (both available as cheap little boards with presoldered headers on aliexpress or wherever).

## Features
* It's a clock!

## Wire-up
Wire up is pretty simple if you want to build one of these:
* Arduino nano (or really any other arduino with enough free pins -- you'll need 11)
* The DS3231 board wants I2C SDA and SCL lines (27 and 28 on my nano, but will vary by board)
* The TM1637 display also needs two pins for clock and data (I used pins 3 and 4)
* You'll need 5 momentary buttons between ground and pins 5,6,7,8,9 (set, alarm, minus, plus, and snooz respectively)
* An LED in series with a current limiting resistor (220 ohm is fine) connected between ground and pin 13 
* A piezo in series with a ~100-200 ohm resistor connected between ground and pin 2

