# 


# If using flipper megacode... config

```
Frequency: 318000000
Preset:    FuriHalSubGhzPresetOok650Async
Protocol:  MegaCode
Bit:       24
```

```
Frequency:          318000000
Modulation:         ASK/OOK
Packet mode:        asynchronous serial
GDO0:               raw demodulated data
RX bandwidth:       270-650 kHz initially

Expected pulse:     1000 us
Pulse tolerance:    +/- 200 us

Symbol period:      6000 us
Symbol tolerance:   +/- 500 us

Expected data bits: 24
Expected pulses:    25       // sync + 24 data
Frame gap:          > 8000 us
Reset gap:          > 15000-20000 us
```


```
| Parameter                    |                Typical MegaCode setting |
| ---------------------------- | --------------------------------------: |
| Carrier                      |                         **318.000 MHz** |
| Carrier tolerance            |                      about **±100 kHz** |
| Modulation                   |                           **OOK / ASK** |
| Encoding                     |     **Pulse-position modulation (PPM)** |
| RF data bits                 |       **24 bits** after synchronization |
| Bit-frame width              |                             **6000 µs** |
| RF ON pulse                  |                             **1000 µs** |
| Symbol rate                  |                **166.7 bit frames/sec** |
| Sync                         | one **1 ms sync pulse** in a 6 ms frame |
| Data pulses                  |                        24 × 1 ms pulses |
| Complete burst               |                        about **150 ms** |
| Inter-repeat gap             |           typically roughly **8–10 ms** |
| Flipper preset commonly used |                           `Ook650Async` |
| Alternative receive BW       |                AM270 can also work well |

```
