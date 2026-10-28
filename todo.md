# To do

- [ ] **Investigate other TTS engines with better support for other languages.** eSpeak NG
  reads Cyrillic, Greek, kana and Hangul passably, but not kanji: its `ja` voice says
  "Chinese letter" for each one, so the kanji of a Japanese name are read in Mandarin or
  named "Japanese text" (phase 4 of [docs/fixes.md](docs/fixes.md)). Compare engines that
  run offline on the Pi Zero 2 W, for example Piper, RHVoice, Open JTalk (Japanese, reads
  kanji) and Flite, on: which languages they have, how they sound, size, speed and memory
  on the Pi, licence, and whether one utterance can switch voices the way SSML
  `<voice xml:lang>` does now. Related: the kanji reading for `ja` and voices to download
  (open question 4 and step 1.3 in docs/fixes.md).
