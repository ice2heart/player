#!/bin/sh
# Generates a small spoken test library with espeak-ng + ffmpeg:
#   library/Sample Book (folder)/01..03.mp3  -> one file per chapter
#   library/Chaptered Story.m4b              -> one file with embedded chapters
#   music/Tones/01..03.mp3                   -> an album
#   music/Short Melody.mp3                   -> a loose track
# Usage: make_sample_library.sh [LIBRARY_DIR] [MUSIC_DIR]
set -e

OUT="${1:-library}"
MUSIC="${2:-music}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

speak() { # text, output file
    espeak-ng -s 160 -w "$TMP/speech.wav" "$1"
    ffmpeg -loglevel error -y -i "$TMP/speech.wav" "$2"
}

paragraph() { # chapter number
    n="$1"
    text="This is chapter $n."
    i=1
    while [ $i -le 12 ]; do
        text="$text Sentence $i of chapter $n. The quick brown fox jumps over the lazy dog."
        i=$((i + 1))
    done
    echo "$text"
}

mkdir -p "$OUT/Sample Book"
for n in 1 2 3; do
    speak "$(paragraph $n)" "$OUT/Sample Book/0$n Chapter $n.mp3"
done

# Single file with chapter markers.
: > "$TMP/list.txt"
printf ';FFMETADATA1\ntitle=Chaptered Story\n' > "$TMP/meta.txt"
start=0
for n in 1 2 3 4; do
    speak "$(paragraph $n)" "$TMP/part$n.wav"
    echo "file '$TMP/part$n.wav'" >> "$TMP/list.txt"
    ms=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$TMP/part$n.wav" | awk '{printf "%d", $1 * 1000}')
    printf '[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\ntitle=Part %d\n' "$start" $((start + ms)) "$n" >> "$TMP/meta.txt"
    start=$((start + ms))
done
ffmpeg -loglevel error -y -f concat -safe 0 -i "$TMP/list.txt" -i "$TMP/meta.txt" \
    -map_metadata 1 -map_chapters 1 -c:a aac -b:a 64k "$OUT/Chaptered Story.m4b"

# Music: sine tones stand in for songs.
tone() { # frequency, seconds, output file
    ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=$1:duration=$2" -ac 2 -b:a 96k "$3"
}
mkdir -p "$MUSIC/Tones"
tone 220 20 "$MUSIC/Tones/01 Low.mp3"
tone 440 20 "$MUSIC/Tones/02 Middle.mp3"
tone 880 20 "$MUSIC/Tones/03 High.mp3"
ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=330:duration=4" -f lavfi -i "sine=frequency=392:duration=4" \
    -filter_complex "[0][1]concat=n=2:v=0:a=1" -ac 2 -b:a 96k "$MUSIC/Short Melody.mp3"

echo "Sample library written to $OUT, music to $MUSIC"
