# iOS FFplay sound, progress, and assistant entry (2026-10-01)

## Symptoms and causes

- Video popup rendered frames but could be silent. FFplay already decoded AAC and
  submitted PCM to AudioQueue, but the iOS popup never configured AVAudioSession.
  The default session follows the Silent switch, and a prior recording session
  can leave the app in a category that does not play through the speaker.
- The slider never moved because it only sent seek commands. None of the mobile
  views read FFplay's master clock or container duration.
- An iOS video control could become unavailable while the popup was still
  visible. `viewWillDisappear` stopped playback even for a temporary covering
  view, and another `viewDidAppear` stopped the same controller again.
- Sending dictated text in AI Assistant could leave the timeline blank; the
  same state was also visible after entering the conversation. Its own
  `LazyVStack` followed a transparent end marker but did not restore position
  when message content gained height after the first layout. This was separate
  from the earlier IM conversation fix.

## Changes and validation

- The iOS popup activates `.playback` / `.moviePlayback` before starting FFplay.
  An activation failure is shown in the popup.
- A shared playback status is sampled on FFplay's event thread without editing
  `ffplay.c`; iOS and Android use it for the slider and elapsed/duration label.
- The iOS controller remains active across temporary view disappearance and
  logs a specific reason when Agent playback control is unavailable.
- The AI Assistant timeline now follows its last real message through delayed
  content layout, using the scroll observer already verified for IM history.
- macOS hosted-FFplay regression used an AAC fixture at normal volume and
  verified non-silent PCM was queued and the playback clock advanced. iOS
  simulator Release and Android Debug builds passed. iOS simulator playback
  screenshot showed the elapsed label reach `0:02 / 0:02`.

## Remaining device check

The iPhone was not connected for audio-output testing. A physical device should
verify speaker routing with the Silent switch on and after using voice input.
