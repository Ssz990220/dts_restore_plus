import copy
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

source = Path(__file__).resolve().parents[2] / 'pcm-mat-watch.py'
spec = importlib.util.spec_from_file_location('pcm_policy', source)
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.row = dict(type='media', currentAppId='moonfin', is_foreground=True,
                        resource=[dict(resource='ADEC')], pid=123, timestamp=100000000000,
                        id='test-pipeline')
        self.state = dict(media_state='play', audioInfo=dict(track=0), sourceInfo=dict(
            programInfo=[dict(audioTrackInfo=[dict(codec='dts', channels=6, sampleRate=48)])]))

    def test_dts_stream_and_selected_track(self):
        self.assertIsNotNone(p.candidate(self.row, self.state, 'moonfin'))
        for field, value in [('codec', 'ac3'), ('channels', 2), ('channels', 8),
                             ('sampleRate', 96)]:
            state = copy.deepcopy(self.state)
            state['sourceInfo']['programInfo'][0]['audioTrackInfo'][0][field] = value
            self.assertIsNone(p.candidate(self.row, state, 'moonfin'))
        self.state['audioInfo']['track'] = 9
        self.assertIsNone(p.candidate(self.row, self.state, 'moonfin'))

    def test_foreground_playing_resource_required(self):
        self.assertIsNone(p.candidate(self.row, self.state, 'other-app'))
        self.assertIsNone(p.candidate(self.row, self.state, None))
        for field, value in [('is_foreground', False), ('resource', [])]:
            row = dict(self.row, **{field: value})
            self.assertIsNone(p.candidate(row, self.state, 'moonfin'))
        self.state['media_state'] = 'pause'
        self.assertIsNone(p.candidate(self.row, self.state, 'moonfin'))

    def test_sink_proof_must_match_pid_and_current_pipeline(self):
        flag = '[ 100.123] lgao NOT (00123:00567) lgao_set_pcm_multi_channel changing 0 -> 1'
        converter = ('[ 100.130] Pcmcvt_Event.1332 channels=(int)8, '
                     'bits-per-sample=(int)32, acmod=(int)7')
        log = flag + '\n' + converter
        self.assertTrue(p.sink_proven(log, 123, 100))
        self.assertFalse(p.sink_proven(flag, 123, 100))
        self.assertFalse(p.sink_proven(converter, 123, 100))
        self.assertFalse(p.sink_proven(log.replace('channels=(int)8', 'channels=(int)2'), 123, 100))
        self.assertFalse(p.sink_proven(log.replace('acmod=(int)7', 'acmod=(int)2'), 123, 100))
        self.assertFalse(p.sink_proven(log.replace('100.130', '90.130'), 123, 100))
        self.assertFalse(p.sink_proven(log.replace('100.130', '102.130'), 123, 100))
        self.assertFalse(p.sink_proven(log, 123, 102))
        self.assertFalse(p.sink_proven(log, 12, 100))
        self.assertFalse(p.sink_proven(log.replace('0 -> 1', '1 -> 0'), 123, 100))
        self.assertFalse(p.sink_proven(log.replace('multi_channel', 'bypass'), 123, 100))

    def test_mat_receiver_and_auto_earc_are_required(self):
        sound = dict(soundOutput='external_arc', soundOutputDigital='auto', eArcSupport='on')
        receiver = dict(arcActivation='eARC', earcCapability=['PCM', 'MAT2PCM'])
        self.assertTrue(p.eligible(sound, receiver))
        self.assertFalse(p.eligible(dict(sound, soundOutputDigital='passThrough'), receiver))
        self.assertFalse(p.eligible(dict(sound, soundOutput='tv_speaker'), receiver))
        self.assertFalse(p.eligible(sound, dict(receiver, arcActivation='ARC')))
        self.assertFalse(p.eligible(sound, dict(receiver, earcCapability=['PCM'])))

    def test_restore_does_not_copy_old_channel_count_to_new_app(self):
        previous = dict(isOTTEnable=False, isATMOSLockingEnable=False)
        stream = dict(app='moonfin')
        with patch.object(p, 'quality', return_value=previous), \
                patch.object(p, 'earc', return_value=[2, 1, 0, 0, 1]) as control, \
                patch.object(p, 'set_ott') as ott, \
                patch.object(p, 'foreground', return_value='hdmi1'), \
                patch.object(p, 'settings', return_value=dict(soundOutput='external_arc',
                    soundOutputDigital='auto', eArcSupport='on')):
            session = p.Session(stream)
            control.reset_mock()
            session.changed = True
            session.restore()
            ott.assert_called_once_with(previous, 'moonfin')
            control.assert_not_called()


if __name__ == '__main__':
    unittest.main()
