# How inFAMOUS Second Son went from 15 to almost 30 frames per second on a Steam Deck

*An explanation for people who don't write code, but like knowing how things work.*

---

## The puzzle

inFAMOUS Second Son was made for the PlayStation 4. On the Steam Deck it runs through **shadPS4**, an emulator: a program that pretends to be a PS4 so the game doesn't notice it's somewhere else.

It worked, but slowly. In one busy free-roam scene the game ran at about **15 frames per second**. That's playable, but you feel the sluggishness in every camera turn.

The odd part was what the Steam overlay showed: **CPU and GPU usage both low**. If the machine wasn't busy, why wasn't the game running faster?

It's like a restaurant that's always half-empty yet still has a long wait for food. The cooks aren't overworked and the tables aren't full. Something in between is slow.

---

## Step 1: Who's actually busy?

The overlay shows an *average* over the Deck's 8 processor threads. One fully busy thread barely moves an average of 8, so we looked at each thread on its own.

One thread stood out: the game's **main thread**, the one that runs the game's logic and decides what to draw, sat at **95%** of a core. So the game had a bottleneck after all, just hidden by the averaging.

Next question: what was that thread *doing* with all that time?

---

## Step 2: The main thread was waiting, not working

A *profiler* takes thousands of snapshots per second of where a program is. Ours showed that **over 80% of the main thread's time** went to one tiny loop of instructions. Translated, the loop says:

> "Is the graphics chip done yet? No? Wait 50 microseconds. Is it done yet? No? Wait 50 microseconds…"

The main thread was **standing at the kitchen pass, asking every few seconds whether the plate was ready**. Waiting this way still counts as "busy" for the CPU, which is why the thread showed 95%.

So the real question was: **why was "done" taking so long to arrive?**

---

## Step 3: Follow the "done" message

On a real PS4, when the graphics chip finishes some work, it writes a short note into memory: *"Finished job #1234."* The game watches for that note. We'll call it a **"done" note**. (Technically it's called a *fence*.)

On the Deck the emulator has to produce these notes itself. It does that **honestly**: it only writes the note after the Deck's graphics chip has *really* finished, because writing it early can confuse the game or even crash it.

One small helper thread in the emulator delivers these notes. It handles its jobs **strictly in order**, like one postal worker with one queue.

That same worker had a second job. Sometimes the game wants a finished image back in its own memory, for example to check something it just drew. So the worker also **carried freshly rendered images back** into the game's memory.

Those images were large, and here was the trap.

---

## Step 4: The postal worker and the locked doors

Each image copy was very slow: about **11 milliseconds**, three or four times a frame. A plain memory copy that size should take well under a millisecond.

The reason was how that memory is set up. To make the game faster earlier, guest memory had been placed in a special shared area that both the processor and the graphics chip can reach. That's convenient, but when the processor writes into it, the operating system steps in again and again. While copying an image, the worker had to stop at **every 4 KB of data**: about **115,000 interruptions per second**.

Picture a postal worker carrying a big parcel down a hallway with a door every few steps. Each door needs a key and a signature. The parcel itself is nothing, but the doors take ages.

The "done" notes were tiny envelopes, but they sat in the **same queue, behind the parcels**. So:

1. The graphics chip finishes the frame's work.
2. The "done" note waits behind 2–3 slow parcels.
3. Meanwhile the game's main thread keeps asking "done yet?".
4. Everything else idles, which is why the overlay showed low usage.

The machine had plenty of power. Most of each frame went to waiting for one overloaded worker in one hallway.

---

## Step 5: The first idea, and why we didn't keep it

The obvious fix was to give the "done" notes their own worker so they never wait behind parcels. We tried it, and the game jumped to **24 fps**.

But it broke a promise. The game might read an image right after it gets the "done" note. If the note can overtake the parcel, the game could find the image half-delivered or missing. That's how you get flickering, glitches or crashes.

So we kept the order and asked a different question: **why carry the parcels by hand at all?**

---

## Step 6: The real fix: let the kitchen deliver straight to the table

The shared memory area is reachable by the graphics chip too. That was the whole point of setting it up.

So instead of this:

> graphics chip draws the image → puts it in a temporary box → the worker carries it by hand through all the doors into the game's memory

the emulator now does this:

> graphics chip draws the image → **the graphics chip copies it straight into the game's memory itself**

The graphics chip has no doors in its way. It moves data at full speed, as part of the work it was already doing. When it finishes, the image is already in place, and the "done" note can be delivered right away, **still strictly in order**.

The postal worker went from **55% busy to 0.2% busy**. Its queue only holds small envelopes now.

---

## The result

| | Before | After |
|---|---|---|
| Frames per second (same save, same spot) | **15.5** | **27–28.5** |
| Time per frame | ~65 ms | ~36 ms |
| Helper thread busy | 55% | 0.2% |
| Interruptions per second while copying | ~115,000 | none |

That's **about 75–80% more frames**, from roughly 90 lines of code. No visual setting was lowered, nothing was skipped, and nothing specific to this game was added. The emulator just moves data the way the hardware allows.

---

## Why this was hard to see

- **Low usage didn't mean nothing was wrong.** The machine wasn't overworked. It was waiting on itself.
- **The "busy" thread wasn't doing work.** Asking "done yet?" over and over counts as busy.
- **The slow part looked minor.** Copying images back took about 1% of the graphics chip's time, so earlier measurements reasonably called it harmless. What mattered was *where* the waiting happened: in the one queue the game was watching.
- **The obvious fix was the wrong one.** Letting the "done" notes skip the queue was faster but unsafe. The better fix removed the slow work instead of reordering it.

---

## Update, the same afternoon: 28 → 29.6

Steps 1 and 2 of the plan were done:

- **Re-tuned and profiled.** The busiest part of the graphics thread kept asking "is this piece of memory ready for the graphics chip?" before every drawing command. It answered by checking a checklist one line per megabyte, thousands of times per frame. Now there's a summary checklist too: one tick means "this whole block of 64 lines is done", so long ranges take a few checks instead of hundreds. Result: **27.7 → 29.6 fps** in the benchmark scene, which now often touches the game's 30 fps cap.
- **A second benchmark scene.** Your newer save point is heavier (about 26 fps), so it's now the main yardstick. The first one is too close to the cap to show further gains.
- **An idea that didn't pan out.** The Bloodborne port has the graphics chip write the game's "frame done" notes itself, instead of a helper thread writing them a moment later. It worked, but made no difference here: those notes weren't arriving late after all.

## Why "steady fps" can still feel choppy

The game shows a new picture on every second screen refresh, which is 30 times a second. If a frame isn't ready in time, it doesn't show a little late: it misses the slot entirely and waits for the next one, so that frame stays on screen twice as long (67 ms instead of 33 ms). At "26 fps" about 1 frame in 6 is one of those doubled frames, and that's the judder you can feel even though the counter looks steady. The emulator now measures this directly. Every speed-up removes doubled frames, and at a true 30 they disappear.

A different kind of hitch happens when you reach a new area and the emulator has to prepare new graphics programs. The Bloodborne port prepares those in the background, and that's on the list too.

## Unlocking the frame rate, and a free 7%

The game's own menu had its frame rate locked to 30. With the lock, a frame that's even slightly late waits for the next 30 fps slot, so it stays on screen twice as long. With the lock off, a late frame just shows a little late. Unlocked, the benchmark scene runs at about 30.6 fps and your save at about 26, and frames never take longer than 50 ms.

Measuring again showed something new: the Deck's graphics chip was now busy 88% of the time but running at only 1040 MHz of its possible 1600. Earlier, when the emulator was the bottleneck, the clock made no difference. Now it does: **locking the GPU clock at 1600 MHz** (Steam button → Performance → Manual GPU Clock) gives **+7%**, about 33 and 27.6 fps. At 1600 MHz the chip has spare time again, which points back at the emulator's graphics thread as the next thing to speed up.

## What's next

With this wait gone, the next bottleneck is in a different place. The plan:

1. **Split the emulator's main graphics thread into two stages**, like an assembly line, so translating the PS4's drawing commands overlaps with sending them to the Deck's graphics chip. Another fan-made Bloodborne port proved this design on the same hardware. We measured that it fits this game too: the game switches between drawing and its other GPU work only about 7 times per frame, so the two stages would rarely have to wait for each other.
2. **Background preparation of new graphics programs**, against the hitches in new areas.

The goal is a steady **30 frames per second** for a game that deserves it.
