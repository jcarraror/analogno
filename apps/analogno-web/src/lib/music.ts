import type { SoundfontPreset } from "../types/runtime";

export const GM_PROGRAMS: string[] = [
  "Acoustic Grand Piano","Bright Acoustic Piano","Electric Grand Piano","Honky-tonk Piano",
  "Electric Piano 1","Electric Piano 2","Harpsichord","Clavinet",
  "Celesta","Glockenspiel","Music Box","Vibraphone","Marimba","Xylophone","Tubular Bells","Dulcimer",
  "Drawbar Organ","Percussive Organ","Rock Organ","Church Organ","Reed Organ","Accordion","Harmonica","Tango Accordion",
  "Nylon Guitar","Steel Guitar","Jazz Guitar","Clean Guitar","Muted Guitar","Overdriven Guitar","Distortion Guitar","Guitar Harmonics",
  "Acoustic Bass","Finger Bass","Pick Bass","Fretless Bass","Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2",
  "Violin","Viola","Cello","Contrabass","Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani",
  "String Ensemble 1","String Ensemble 2","Synth Strings 1","Synth Strings 2","Choir Aahs","Voice Oohs","Synth Voice","Orchestra Hit",
  "Trumpet","Trombone","Tuba","Muted Trumpet","French Horn","Brass Section","Synth Brass 1","Synth Brass 2",
  "Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax","Oboe","English Horn","Bassoon","Clarinet",
  "Piccolo","Flute","Recorder","Pan Flute","Blown Bottle","Shakuhachi","Whistle","Ocarina",
  "Lead 1 (square)","Lead 2 (sawtooth)","Lead 3 (calliope)","Lead 4 (chiff)",
  "Lead 5 (charang)","Lead 6 (voice)","Lead 7 (fifths)","Lead 8 (bass+lead)",
  "Pad 1 (new age)","Pad 2 (warm)","Pad 3 (polysynth)","Pad 4 (choir)",
  "Pad 5 (bowed)","Pad 6 (metallic)","Pad 7 (halo)","Pad 8 (sweep)",
  "FX 1 (rain)","FX 2 (soundtrack)","FX 3 (crystal)","FX 4 (atmosphere)",
  "FX 5 (brightness)","FX 6 (goblins)","FX 7 (echoes)","FX 8 (sci-fi)",
  "Sitar","Banjo","Shamisen","Koto","Kalimba","Bagpipe","Fiddle","Shanai",
  "Tinkle Bell","Agogo","Steel Drums","Woodblock","Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal",
  "Guitar Fret Noise","Breath Noise","Seashore","Bird Tweet","Telephone Ring","Helicopter","Applause","Gunshot",
];

export const SCALE_SEMITONES: Record<string, number[]> = {
  minor_pentatonic: [0, 3, 5, 7, 10],
  major: [0, 2, 4, 5, 7, 9, 11],
  natural_minor: [0, 2, 3, 5, 7, 8, 10],
  dorian: [0, 2, 3, 5, 7, 9, 10],
  phrygian: [0, 1, 3, 5, 7, 8, 10],
  chromatic: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
};

export function formatNumber(value: number): string {
  return value.toFixed(3);
}

export function midiNoteName(note: number): string {
  const names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
  const octave = Math.floor(note / 12) - 1;
  return `${names[note % 12]}${octave}`;
}

export function soundfontName(path: string): string {
  const file = path.split(/[\\/]/).pop() ?? path;
  return file.replace(/\.[^.]+$/i, "") || path;
}

export function soundfontFormat(path: string): string {
  const file = path.split(/[\\/]/).pop() ?? path;
  const match = file.match(/\.([^.]+)$/);
  return match?.[1]?.toUpperCase() ?? "Soundfont";
}

export function pluralize(count: number, singular: string): string {
  return `${count} ${singular}${count === 1 ? "" : "s"}`;
}

export function bankLabel(bank: number): string {
  return `Bank ${bank}`;
}

export function bankRole(bank: number): string {
  return bank === 128 ? "percussion" : "melodic";
}

export function patchName(
  bank: number,
  program: number,
  presets: SoundfontPreset[],
): string {
  return presets.find(p => p.bank === bank && p.program === program)?.name
    ?? GM_PROGRAMS[program]
    ?? `Program ${program + 1}`;
}

export function stepNoteName(degree: number, rootMidi: number, scaleName: string): string {
  const semitones = SCALE_SEMITONES[scaleName] ?? SCALE_SEMITONES.major;
  const idx = ((degree % semitones.length) + semitones.length) % semitones.length;
  const octaveBonus = Math.floor(degree / semitones.length);
  const midi = rootMidi + semitones[idx] + octaveBonus * 12;
  return midiNoteName(Math.max(0, Math.min(127, midi)));
}
