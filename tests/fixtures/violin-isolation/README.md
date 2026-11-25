# Violin isolation test recordings

The CLI test mixes clips from three Creative Commons CC0 recordings. The
reference is the violin excerpt at the gain and sample positions used in the
mix.

| Fixture | Recording | Creator | Kept samples |
| --- | --- | --- | --- |
| `violin.wav.gz` | [Baroque 05 - Phrasing 3.wav](https://freesound.org/people/Cunningar0807/sounds/401897/) | Cunningar0807 | 158760 to 213885 |
| `cough.wav.gz` | [Cough (2)](https://freesound.org/people/OwlStorm/sounds/151212/) | OwlStorm (Ashe Kirk) | 1102 to 8820 |
| `step.wav.gz` | [Footstep on wood](https://freesound.org/people/SoundsAreGr8/sounds/340983/) | SoundsAreGr8 | 0 to 7720 |

Each fixture uses the recording's high-quality MP3 preview. Decode it with
`ffmpeg -i preview.mp3 -ac 1 -ar 22050 -c:a pcm_s16le decoded.wav`, then
keep the half-open sample range in the table and gzip the resulting WAVE
file. [CC0 1.0 terms](https://creativecommons.org/publicdomain/zero/1.0/)
