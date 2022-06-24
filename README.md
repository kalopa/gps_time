# GPS Time

Set the system time by reading from a GPS device connected via serial/USB.

This code opens a serial device at a specified baud rate,
then trawls through the data received looking for a $GPRMC message
from the GPS.
This particular message includes not just the time but also the date.
A sample message might be:

*$GPRMC,211321.000,A,5309.7743,N,01204.5576,W,0.17,78.41,200813,,,A*4E*

The second field has the time in the form **HHMMSS.NNN** where N
is the time in milliseconds.
The tenth field has the date in the form YYMMDD in a not particularly
Y2K-friendly format.
