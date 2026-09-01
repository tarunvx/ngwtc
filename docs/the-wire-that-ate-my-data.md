# The Wire That Ate a Quarter of My Data

**How a 15-metre serial cable quietly threw away 24% of everything it carried, and what it took to find out why.**

---

There is a particular kind of bug that doesn't crash anything. Nothing catches
fire. No error appears in a log. The system just... works a bit worse than it
should, in a way nobody can quite put their finger on.

This is the story of one of those.

---

## The setup

I have a water tank on my roof and a pump 250 feet down a borewell. Between them
sits a controller that decides when to run the pump, and up at the tank there's a
second little board that watches the water level, measures flow, and reports back.

The two boards talk over a wire. Just an ordinary serial connection — the same
thing that's been carrying data between computers since the 1960s. Three
conductors running about fifteen metres: power, ground, and one signal wire. No
shielding. No fancy transceivers. Just wire.

The little board up at the tank sends a small 26-byte report four times a second.
Water level, distance to the surface, how much has flowed through. That's it.

It worked. Data arrived. Numbers appeared on the display.

And roughly fifteen times every half hour, the controller announced that the tank
sensor had gone away — then, a moment later, that it was back.

---

## The thing that looked obviously true

My first theory was the obvious one, and I was confident about it.

The little board at the tank is at the far end of fifteen metres of thin wire. Of
course it's browning out. The voltage sags, the board resets, and for a second or
two it stops talking. That's why the link keeps dropping. I'd even set the power
supply to 5.5 volts instead of 5 to compensate for the voltage lost along the
cable, so I already knew that cable was costing me something.

I was ready to order a fistful of capacitors and call it solved.

Then I actually looked.

---

## Every message is numbered

Here's the single cheapest thing you can do in a system like this, and it costs
you two bytes: **put a counter in every message.**

Message number 1, message number 2, message number 3. That's all. The receiver
watches those numbers go by.

It turns out this one field can answer questions you didn't know you were asking.
Because if the sender ever restarts, its counter goes back to zero. There's no
hiding it.

So I watched the counter for half an hour. It went from 30 to 8,995. Never
stalled. Never jumped. Never — not once — went back to zero.

That board wasn't crashing. It had been running perfectly the entire time.

My capacitors would have been the right components fitted for entirely the wrong
reason.

And here's the second thing the counter tells you for free. If you receive
message 100 and then message 103, you know exactly what happened to 101 and 102:
they left, and they never arrived.

Counting those gaps gave me the real number.

```
2149 messages lost out of 8965 sent  =  24%
```

Nearly a quarter of everything that board sent me was vanishing somewhere in
fifteen metres of wire.

---

## Where does a quarter of your data go?

A message is 26 bytes. It only survives if all 26 arrive intact. So if 76% of
messages are getting through, I can work backwards to how often an individual
byte is being mangled.

```
0.76 to the power of (1/26)  =  0.9895

about 1 byte in 100 arrives corrupted
```

One percent.

If that number doesn't sound alarming, it should. This is a *wire*. Two boards,
a piece of copper between them, nothing else involved. A serial link like this
should run for months without a single corrupted byte. One in a hundred is
catastrophically bad.

Something was actively scrambling my data. And it clearly wasn't the sender,
because the sender was demonstrably fine.

---

## The part that took me longest to see

Here's the thing I should have realised much earlier.

When the receiving chip looks at that signal wire to decide whether it's seeing a
1 or a 0, it doesn't have some absolute notion of what a volt is. **It compares
the signal against its own ground.** Ground is the reference. Zero. The thing
everything else is measured against.

Below about 0.8 volts, it calls that a 0. Above about 2.5 volts, a 1.

Now look at my wiring again. The tank board's ground wire runs fifteen metres
back to the controller — and *that same wire is how all of the tank board's
power gets back*. Every milliamp that board consumes goes home through the exact
piece of copper that my signal is measured against.

Copper isn't free. Fifteen metres of thin wire has real resistance — call it 1.26
ohms. Push 150 milliamps through it and you get:

```
0.150 amps  x  1.26 ohms  =  0.19 volts
```

So the tank board's idea of "ground" sits about 190 millivolts above the
controller's idea of "ground". The board sends a 0 as zero volts; it arrives as
0.19 volts. Still under the 0.8 volt threshold, so it's still read as a 0.

Fine. Except.

**That 190 millivolts isn't a fixed number. It's current times resistance — and
the current keeps changing.**

The tank board has an ultrasonic sensor that fires a burst to measure the water
surface. Every burst is a gulp of current. Every gulp yanks that shared ground
wire. And the ground wire is the reference the receiver is using to decide
whether each bit is a 1 or a 0 — *while the message is still going down the wire*.

The measuring stick was wobbling while I was measuring with it.

That's not a bug in any code. That's the topology.

---

## Fixing it, five ways

None of these needed new chips. All of them cost pennies.

### 1. Give the burst somewhere closer to come from

If pulling current through fifteen metres of wire is what shakes the ground, then
the fix is to not pull it through fifteen metres of wire.

A big capacitor right next to the tank board — 1000 µF — acts as a local
reservoir. When the ultrasonic sensor fires, it drinks from the capacitor two
centimetres away, not from the supply fifteen metres back. The long wire only has
to trickle-charge the capacitor between bursts.

**One detail people skip:** put a small 0.1 µF ceramic capacitor next to the big
one. The big electrolytic is good at storing energy but genuinely poor at
responding to fast changes — it has internal resistance and inductance that make
it sluggish above a few hundred kHz. And fast changes are exactly what's coupling
into your ground. Big capacitor for the bulk, small one for the speed. Always
both.

### 2. A diode in the power line — and absolutely not in the ground

I added a Schottky diode in the incoming power line. It stops my nice local
capacitor from draining backwards up the wire when something else on that supply
takes a gulp, and it protects against wiring the thing up backwards.

Should you think whether to put one in the ground line for symmetry?

**Don't ever do this.** A diode drops voltage, and how much it drops depends on
how much current flows through it. So a diode in the ground line would lift the
tank board's ground above the controller's ground — by an amount that changes
with load.

That is *exactly* the problem described above, deliberately installed. Ground
must be one clean, shared, low-resistance reference. It's the one wire you don't
get clever with.

### 3. Slow down

This is the one that feels wrong and works best.

I dropped the data rate from 9600 bits per second to 2400. Four times slower.

At 9600 baud, each bit occupies 104 microseconds of wire time. At 2400, each bit
gets 417 microseconds. The receiver checks the middle of each bit to decide 1 or
0 — so a wider bit means a wobble in the ground has to last four times longer
before it can push that sampling point onto the wrong side of the line.

Slower is more robust. Every microsecond of extra bit width is margin.

And it cost me nothing, because I was never using the bandwidth. 26 bytes four
times a second needs about 1040 bits per second. I was running at 9600 to be
"fast" on a link that was 90% idle.

**If you're not short of bandwidth, running fast is just throwing away noise
immunity for nothing.**

### 4. Don't shout while you're listening

Then I noticed the ordering in the tank board's code:

```c
usUpdate();     // fire the ultrasonic burst   <-- current spike
...
Serial.write(); // send the message            <-- immediately after
```

Every single message was being sent in the immediate aftermath of the current
spike I'd just identified as the problem. Worst possible timing, in the same loop.

Easy fix — move them apart. There's 250 milliseconds in the cycle; use it.

**But there's a trap here, and it nearly bit me.**

`Serial.write()` does not send anything. It hands the bytes to the hardware and
returns almost instantly. The chip then clocks those bytes out slowly, in the
background, long after your code has moved on. At 2400 baud, a 26-byte message is
still physically travelling down that wire for **108 milliseconds** after
`write()` returned.

So just reordering the two lines would have achieved precisely nothing. The ping
would still have fired in the middle of the transmission — the code would just
*look* correct.

The fix needs an explicit "wait until it's actually gone":

```c
Serial.write(frame);
Serial.flush();   // NOW the bytes are really on the wire
usUpdate();       // safe to make noise
```

And note the sting: **slowing the baud rate made this worse.** At 9600, the
transmission took 27 ms and the overlap was a maybe. At 2400 it takes 108 ms and
the overlap was a certainty. Two sensible improvements that sabotage each other
if you only apply one.

### 5. Unconnected pins are antennas

Last one, and it's the most entertaining.

While testing, I unplugged the flow meter. The system promptly reported water
flowing at **127 litres per minute** — through a pump that was switched off.

The flow meter's input pin was being held up by a weak internal resistor inside
the chip, somewhere in the 30–100 kΩ range. That's a very light grip. Attach a
length of wire to a pin held that weakly and you haven't got an input — you've
got an aerial. It was picking up electrical noise from the surroundings and the
chip was dutifully counting about 511 phantom pulses per second.

A single 10 kΩ resistor to the supply grips that pin about ten times harder, and
the ghost water disappeared. With the pump off it now reads a solid 0.0.

**Rule of thumb:** any input pin that isn't being actively driven, on a wire
longer than your finger, wants a real resistor. The ones built into the chip are
for buttons on a circuit board, not for cables.

---

## The part that actually solved it

Here's the twist.

All that work took frame loss from 24% down to 15%. Real progress, genuinely
worthwhile — but if I'm honest, it's not a triumph. One in seven messages is
still going missing.

And yet the dropouts stopped almost completely.

Because the thing that fixed the *symptom* wasn't better wire. It was accepting
that the wire is lossy and asking a different question: **how many messages in a
row do I need to miss before I should genuinely panic?**

The controller had been giving up after one second of silence. At four messages a
second, that's four in a row. Which sounds reasonable — until you do the
arithmetic:

```
0.24 x 0.24 x 0.24 x 0.24  =  1 chance in 300

...and I get 7200 messages every half hour
```

One in 300 sounds rare. At 7200 attempts it predicts about 24 false alarms per
half hour. I was seeing around 15–18.

**Those dropouts were never real.** They were an impatient timeout meeting an
unlucky run of noise. The link wasn't dying fifteen times an hour; my code was
just declaring it dead on flimsy evidence.

Waiting for eight consecutive misses instead of four:

```
0.152 to the power of 8  =  1 chance in 3.5 million

across a whole day of messages  =  roughly one false alarm every ten days
```

Same wire. Same noise. Same loss rate. From 184 false alarms a day to one every
ten days, by changing a single number.

---

## Why that was even allowed to work

Waiting longer only works because of a decision made much earlier, and it's worth
spelling out.

**Every message contains the complete current state, not a description of what
changed.**

The flow meter is the clearest example. It reports a lifetime running total —
"since I booted, I have counted 48,203 pulses" — and the controller works out the
rate by comparing that against the last total it saw.

The alternative would have been to send "37 pulses since my last message". It's
smaller. It seems tidier. And with a 15% loss rate it would have been a disaster:
every lost message would permanently erase the water it described. **My totals
would have been running 15% low, forever, and nothing in the system would ever
have told me.**

With a running total, a lost message costs nothing at all. The next one to arrive
carries the full picture. You can throw away one in seven and still be exactly
right.

That's the whole trick, and it's free if you decide it early. Design so that
losing a message is boring.

---

## Three times my instruments lied to me

Worth including, because each one produced a confident wrong answer.

**The counter that read 65,816.** My "messages lost" counter reported more losses
than messages ever sent. When the tank board restarted, its message number went
back to 0, and my code calculated "0 minus 500" using arithmetic that can't
represent negatives — which produced about 65,000 instead of a negative number.
One reboot injected 65,000 phantom losses in a single step. Now a gap that large
is recognised for what it is and counted as a restart, which turned out to be
more useful than the number I originally wanted.

**The crash that never happened.** The board reported that its last restart was
caused by a watchdog timeout. It hadn't restarted at all — I'd just reflashed it
over USB, and the programming tool resets the chip using the watchdog. *The first
restart reason after any USB flash is your programmer, not a fault.*

**The noise that was arithmetic.** My flow reading jittered between 2.8 and 3.8
litres per minute and looked exactly like electrical interference. It wasn't. At
3 litres per minute, each 250 ms sample contained fewer than six pulses — so one
pulse either way *is* an 18% swing. There was nothing to filter. I was measuring
with too coarse a ruler. Counting over three seconds instead gave 67 pulses per
sample and the jitter fell to 1.5%. When a reading looks noisy, check whether you
have enough to count before reaching for an average.

---

## What I didn't do

The textbook answer to all of this is RS-485. It uses two wires and compares them
against *each other* instead of against ground — which makes it completely immune
to the wandering-ground problem at the heart of this story. It would probably have
taken my losses to zero.

I didn't fit it, because I didn't need to. Everything above cost small change and
added no new parts to fail.

But I'd reach for it without hesitation if the run were much longer than fifteen
metres, if I couldn't keep signal and power grounds apart, if the environment
were electrically nastier, or if I needed *guaranteed* delivery rather than
best-effort readings.

There's also a middle option I'd take instantly if I were pulling fresh cable:
**one extra wire, used only as a signal ground, carrying no power current at
all.** That eliminates the coupling completely instead of merely damping it. One
conductor, problem gone.

---

## What I'd tell my past self

**Measure before you theorise.** I was ready to order capacitors for a
power problem that a single counter proved didn't exist.

**Number your messages.** Two bytes. It told me the sender never crashed, told me
exactly how many messages were lost, and eventually told me when the sender *did*
restart.

**Ground isn't free.** If the same wire carries your return current and your
signal reference, your data quality is at the mercy of your power consumption.

**Slow down if you can afford to.** Bandwidth you're not using is noise immunity
you're refusing.

**Know when your bytes are actually gone.** `write()` returning doesn't mean
anything left the building.

**Tie down your inputs.** A floating pin on a long wire will invent data for you,
and it will be very confident about it.

**And most of all: design so that losing a message is boring.** I spent days
chasing the error rate down by a third. What actually made the system dependable
was accepting the errors that remained and making them not matter.

---

*The wire still drops one message in seven. I've stopped trying to fix that, and
the system hasn't noticed in weeks.*
