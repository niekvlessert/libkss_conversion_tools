This directory is a bit messy, some information.

Some goals I had kind of, using AI:

- Playing KSS files on MSX
- Getting SNG files in KSS files
- And a bit later; adding compression to the files with ZX0 compression.
- Adding Moonsound support to libkss
- Putting MWM/MWK files in KSS files
- Adding more metadata to KSS.

It took me a while and I walked into several directions.

I started with the SNG thing; I used the Roboplay source to be able to create a c expansion to libkss that makes it posibble to play SNG. It includes all SNG features (from Roboplay, thanks!) and also the hex code that is the player for MSX in the KSS file.

I then asked if the Libkss creator, the amazing okaxaki, if he wanted to include that in a pull request. 

While I was waiting for an answer I continued; what libkss does not do is output the actual KSS on disk afaik.

I realized it's not that important to have the actual support of SNG in libkss itself; if I created the KSS files, they would just work on any KSS player, since it emulates the Z80, some chips and parts of the MSX, it just executes the player engine like an MSX (or Sega) would, which plays the audio.

While I was waiting for an answer I continued; I wanted to put more SNG files in a single KSS file. That worked fine; bigger then 1 MB gave me trouble though, I've put all SNG music on the file-hunter site in 3 KSS files, tested on stock Libkss. Nice.

But I needed to be able to play the files, so I created a player for Mac based on libkss.

But then I learned about ZX0 compression; compression on a modern PC, which is quite heavy on the compression CPU, but still peanuts, and decompression is then just 250 bytes of Z80 assembly, which runs really quick on a Z80. Awesome!

I was then also working on Pro tracker to KSS, because why not? :) So then I thought; why not let the Z80 execute the decompression in the KSS file, which then decompresses the engine and the music, which then starts playing. So I ended up with a Protracker file of 100 kB with over 100 tracks!

But then I was thinking if I could not have 2 engines in the same KSS file? So after some trying I now have a KSS file with almost all Pro tracker music on the file-hunter site AND all SNG music, compressed in a file under 1 MB. :) Why not all music? Because I passed 256 tracks... I left out some short experimental Protracker files. The file plays fine on a stock KSS player afaik, but not tested well.

But then I heard from oaxaki, he didn't like the method of the implementation I used; the integration of the engine was different then he does, which is actually cleaner, I learned; just include the MSX hex code, build the KSS with that in libkss memory and play it. The whole logic from Roboplay is not required then!

But with my progress I got inspiration; I then reacted to that with more feature suggestions; for example add Moonsound support, add more metadata in the kss format, while maintaining backward compatability.

I learned expanding the format is not something he is willing to do, which is fair in a way, there's many players, the format support is already scattered. So I decided to do my own format; KSP, KSS Plus, since it's all MIT. I'm not finished with the spec yet.

Then I added Moonsound support to libkss and build a mwm2kss tool. I feel like a wizard with AI... :)

But then I wanted to play it on the MSX. Then I remembered about the paging trouble you get when playing KSS on MSX. I realised that's probably why KSSPlay from NYYRIKKI is only for Turbo-R; emulate the Z80, no page problems.

But I thought it could work; player in one page, data in another. Modifications on KSS formats are fine; it's KSP. Got it working eventually including compression. :) More files in a single KSP the MSX was not happy about. I did a DMV pack, but it's not cleanly finished.

Then I moved on to Konami SCC music. Getting that to play on MSX was very difficult; I learned most KSS files have a different approach of using paging and storing the music, I tried a lot but if I realised that if I could one to play the second wouldn't work.

So then I tried another approach; extracting the tracks and engine from the KSS files and make it dynamic, so the player can decide where to put the data in the memory. This STILL proved difficult, since the SCC take a whole memory page.

It might be doable for a good MSX programmer to make it work with a lot of page swapping, but that's not something I can do and the AI models also cannot. ;)

So then I decided again on another approach; I decided to use complete 16 KB page images. Each logical page contains the engine at exactly the same addresses, followed by the music data for a group of songs. The next logical page contains a byte-for-byte identical engine area but a different group of song data.

In the compressed KSP file, the engine and other shared data are stored only once. Each page contains only its page-specific music payload and any required pointer patches. At runtime, the player decompresses or copies the shared engine into a writable mapper page and materializes the required song payload around it. When another logical page is needed, the player keeps or reconstructs the shared engine and replaces only the page-specific portion.

Pretty cool stuff I'd say!

So I created a MSX-DOS2 player which can play those SCC and OPL4 packs. AI and me had a hard time but it works! Though it has issues; for example you can't exit playback of a Moonsound track without rebooting, the KSP files don't support MWK files yet, the title is only showed for the first track which is not accurate for Konami SCC of course, no FMPAC tested, etc.

But mounting the msx directory on openMSX while starting it with '-machine Panasonic_FS-A1ST -ext msxdos2 -extb scc -extc moonsound' works well otherwise I'd say.

kssplay filename.ksp, that's it. Space for next, escape to exit.

Having all Kingsvalley 2 music in 15499 bytes is pretty cool as well I think.

It has bugs and issues, but it's a WIP...

Things I'm thinking about;

- Creating a Moonsound/SCC combined pack of the 1988 Haunted Castle. The Moonsound can handle the Yamaha YM3812 OPL2 and the Konami K007232 (2 channel PCM).
- Embedding Screen 5 full screen image and 1/4 size screen 5 images of the actual game inside the KSS, so a MSX player can show the 1/4 image and track information
- A player for current hardware can upscale the image
- Embedding more meta data like game, publisher, composers, etc.
- Creating more automation in converting KSS files to KSP, so they will work on MSX. For example the Sega Music. Don't know if that's doable.
- ???
