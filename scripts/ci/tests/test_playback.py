"""Unavailable installed codecs must not hide broken decoders or sandbox crashes."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch


SPEC = importlib.util.spec_from_file_location(
    'check_playback', Path(__file__).resolve().parents[1] / 'check-playback.py')
PLAYBACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PLAYBACK)
UNSUPPORTED = (2, 0, 0, 0, 0, 0, b'')
FRAME = (0, 400, 96, 48, 0, 0, b'pixels')


class PlaybackCapabilitiesTest(unittest.TestCase):
    def test_protocol_descriptor_and_eof_with_other_descriptors_open(self):
        with tempfile.TemporaryDirectory() as temporary, open(os.devnull) as occupied:
            self.assertGreaterEqual(occupied.fileno(), 3)
            helper = Path(temporary) / 'helper'
            helper.write_text(f'#!{sys.executable}\n'
                              'import socket, struct\n'
                              's = socket.socket(fileno=3)\n'
                              's.recvmsg(1000, socket.CMSG_SPACE(4))\n'
                              's.send(struct.pack("=IIqIIII", 0x52504C31, 2, 0, 0, 0, 0, 0))\n'
                              'assert s.recv(1000) == b""\n')
            helper.chmod(0o755)
            decoder = PLAYBACK.Decoder(helper, helper)
            self.assertEqual(decoder.step(), UNSUPPORTED)
            decoder.close()

    def run_video(self, available, responses, exit_error=None):
        decoder = Mock()
        decoder.step.side_effect = responses
        decoder.close.side_effect = exit_error
        with patch.object(PLAYBACK, 'Decoder', return_value=decoder):
            PLAYBACK.check_video(Path('/helper'), Path('/red-hevc.mp4'), ['hevc'],
                                 {'hevc': available})
        return decoder

    def test_missing_codec_requires_bounded_unsupported_and_clean_exit(self):
        decoder = self.run_video(False, [UNSUPPORTED])
        decoder.step.assert_called_once_with()
        decoder.close.assert_called_once_with()
        with self.assertRaisesRegex(AssertionError, 'SIGSYS'):
            self.run_video(False, [UNSUPPORTED], AssertionError('SIGSYS'))
        for response in [FRAME, (2, 1, 0, 0, 0, 0, b''), (3, 0, 0, 0, 0, 0, b'')]:
            with self.subTest(response=response), self.assertRaises(AssertionError):
                self.run_video(False, [response])

    def test_available_codec_must_decode_and_seek(self):
        decoder = self.run_video(True, [FRAME, FRAME])
        self.assertEqual(decoder.step.call_args_list, [unittest.mock.call(),
                                                      unittest.mock.call(100, True)])
        for responses in [[UNSUPPORTED], [FRAME, UNSUPPORTED]]:
            with self.subTest(responses=responses), self.assertRaises(AssertionError):
                self.run_video(True, responses)

    def test_discovery_failure_cannot_turn_into_missing_codec(self):
        for output in ['', 'libavcodec.so.62 => not found\n']:
            with self.subTest(output=output), patch.object(PLAYBACK.subprocess, 'run',
                    return_value=subprocess.CompletedProcess([], 0, output)), \
                    self.assertRaisesRegex(RuntimeError, 'Cannot identify'):
                PLAYBACK.installed_codecs(Path('/helper'))
        with patch.object(PLAYBACK.subprocess, 'run',
                          side_effect=subprocess.CalledProcessError(1, ['ldd'])), \
                self.assertRaises(subprocess.CalledProcessError):
            PLAYBACK.installed_codecs(Path('/helper'))
