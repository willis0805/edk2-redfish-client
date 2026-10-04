# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Isolated unit and loopback TCP tests; never connect to a real BMC.

Run from any directory with:
    python -m unittest discover -s <simulator>/tests -v
"""
import base64
import contextlib
import http.client
import io
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

from flask import Flask, Response
from werkzeug.serving import make_server, WSGIRequestHandler

SIMULATOR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIMULATOR))
import redfishProfileSimulator as simulator
from v1sim.redfishURIs import create_app, rfApi_SimpleServer
from v1sim.serviceRoot import RfServiceRoot
from v1sim.serviceVersions import RfServiceVersions

PROFILE = SIMULATOR / 'MockupData' / 'SimpleOcpServerV1'
SYSTEM = '/redfish/v1/Systems/2M220100SL'
BASIC = {'Authorization': 'Basic ' + base64.b64encode(b'admin:pwd123456').decode('ascii')}
TOKEN = {'X-Auth-Token': '123456SESSIONauthcode'}


@contextlib.contextmanager
def working_directory(directory):
    previous = os.getcwd()
    try:
        os.chdir(directory)
        yield
    finally:
        os.chdir(previous)


def load_app(profile=PROFILE):
    # Loading the profile is intentionally verbose in the standalone simulator.
    with contextlib.redirect_stdout(io.StringIO()):
        root = RfServiceRoot(str(profile), os.path.join('redfish', 'v1'))
        versions = RfServiceVersions(str(profile), 'redfish')
    return create_app(root, versions)


class CliTests(unittest.TestCase):
    def run_main(self, *arguments):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return simulator.main(['simulator'] + list(arguments))

    def test_default_profile_is_script_relative_and_host_is_loopback(self):
        with tempfile.TemporaryDirectory() as directory, working_directory(directory):
            options = simulator.parse_options(['simulator'])
        self.assertEqual(options['profile'], str(PROFILE))
        self.assertEqual(options['host'], '127.0.0.1')
        self.assertEqual(options['port'], 5000)

    def test_short_options(self):
        options = simulator.parse_options(['simulator', '-H127.0.0.1', '-P15001', '-p', str(PROFILE)])
        self.assertEqual(options['host'], '127.0.0.1')
        self.assertEqual(options['port'], 15001)
        self.assertEqual(options['profile'], str(PROFILE))

    def test_legacy_and_documented_long_aliases(self):
        for host, port, profile in [('--Host', '--Port', '--profile'),
                                     ('--host', '--port', '--profile_path')]:
            with self.subTest(host=host):
                options = simulator.parse_options(['simulator', host + '=127.0.0.1',
                                                   port + '=15002', profile + '=' + str(PROFILE)])
                self.assertEqual(options['host'], '127.0.0.1')
                self.assertEqual(options['port'], 15002)
                self.assertEqual(options['profile'], str(PROFILE))

    def test_relative_explicit_profile_uses_callers_directory(self):
        with working_directory(SIMULATOR):
            options = simulator.parse_options(['simulator', '-pMockupData/SimpleOcpServerV1'])
        self.assertEqual(options['profile'], str(PROFILE))

    def test_help_and_version_exit_successfully(self):
        for option in ('-h', '--help', '-V', '--Version', '--version'):
            with self.subTest(option=option):
                self.assertEqual(self.run_main(option), 0)

    def test_invalid_cli_arguments_fail_cleanly(self):
        for arguments in [('--Port=nope',), ('-P0',), ('-P-1',), ('-P65536',),
                          ('--Host=',), ('--unknown',), ('extra',), ('-P',)]:
            with self.subTest(arguments=arguments):
                self.assertEqual(self.run_main(*arguments), 2)

    def test_missing_profile_and_empty_profile_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            for path in (directory, os.path.join(directory, 'missing')):
                with self.subTest(path=path):
                    self.assertEqual(self.run_main('--profile=' + path), 2)

    def test_malformed_profile_fails_without_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory)
            (profile / 'redfish' / 'v1').mkdir(parents=True)
            (profile / 'redfish' / 'index.json').write_text('{', encoding='utf-8')
            (profile / 'redfish' / 'v1' / 'index.json').write_text('{}', encoding='utf-8')
            self.assertEqual(self.run_main('-p', str(profile)), 2)

    def test_certificate_pair_can_use_unprivileged_port(self):
        # Parsing only; actual TLS with a valid locally generated certificate is
        # exercised separately by the loopback HTTPS test.
        with tempfile.TemporaryDirectory() as directory:
            cert = Path(directory) / 'test-cert.pem'
            key = Path(directory) / 'test-key.pem'
            cert.touch()
            key.touch()
            for cert_option, key_option in [('-C', '-K'), ('--Cert=', '--Key='),
                                            ('--cert=', '--key=')]:
                with self.subTest(option=cert_option):
                    options = simulator.parse_options(['simulator', '-P8443',
                                                       cert_option + str(cert), key_option + str(key)])
                    self.assertEqual(options['port'], 8443)
                    self.assertEqual(options['cert'], str(cert))
                    self.assertEqual(options['key'], str(key))

    def test_invalid_tls_options_fail(self):
        for arguments in [('-Cmissing.pem',), ('-Kmissing.pem',), ('-P443',),
                          ('-Cmissing.pem', '-Kmissing.pem', '-P8443')]:
            with self.subTest(arguments=arguments):
                self.assertEqual(self.run_main(*arguments), 2)

    def test_default_start_from_other_directory_is_loopback_and_no_debug(self):
        with tempfile.TemporaryDirectory() as directory, working_directory(directory):
            with patch.object(Flask, 'run') as run:
                self.assertEqual(self.run_main(), 0)
        run.assert_called_once_with(host='127.0.0.1', port=5000, debug=False, use_reloader=False)

    def test_tls_wrapper_passes_certificate_pair_and_requested_port(self):
        app = Flask(__name__)
        with patch('v1sim.redfishURIs.create_app', return_value=app), patch.object(app, 'run') as run:
            rfApi_SimpleServer(None, None, port=8443, cert='cert.pem', key='key.pem')
        run.assert_called_once_with(host='127.0.0.1', port=8443, debug=False,
                                    use_reloader=False, ssl_context=('cert.pem', 'key.pem'))

    def test_real_cli_process_reports_errors_and_help(self):
        with tempfile.TemporaryDirectory() as directory:
            for args, expected in [(['--help'], 0), (['--profile=missing'], 2),
                                   (['--Port=invalid'], 2)]:
                with self.subTest(args=args):
                    result = subprocess.run([sys.executable, str(SIMULATOR / 'redfishProfileSimulator.py')] + args,
                                            cwd=directory, capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                    self.assertNotIn('Traceback', result.stderr)


class ConditionalHookTests(unittest.TestCase):
    """Check the legacy hook itself, not unsupported resource preconditions."""
    def setUp(self):
        simulator.install_conditional_etags()
        self.app = Flask(__name__)

        @self.app.route('/conditional', methods=['GET', 'PATCH'])
        @simulator.conditional
        def conditional_response():
            response = Response('test')
            response.set_etag('original')
            if simulator.flask.request.method == 'PATCH':
                response.set_etag('updated')
            return response

        self.client = self.app.test_client()

    def test_hook_is_idempotent_and_safe_outside_request_context(self):
        before = simulator.werkzeug.wrappers.Response.set_etag
        simulator.install_conditional_etags()
        self.assertIs(before, simulator.werkzeug.wrappers.Response.set_etag)
        response = Response()
        response.set_etag('test')
        self.assertEqual(response.get_etag(), ('test', False))

    def test_modern_exception_response_signatures(self):
        for error, expected in [(simulator.NotModified(), 304), (simulator.PreconditionRequired(), 428)]:
            self.assertEqual(error.get_response().status_code, expected)
            self.assertEqual(error.get_response(environ={}, scope=None).status_code, expected)

    def test_conditional_get_works_with_current_flask(self):
        result = self.client.get('/conditional', headers={'If-None-Match': '"original"'})
        self.assertEqual(result.status_code, 304)
        self.assertEqual(result.data, b'')

    def test_conditional_patch_requires_matching_etag(self):
        self.assertEqual(self.client.patch('/conditional').status_code, 428)
        self.assertEqual(self.client.patch('/conditional', headers={'If-Match': '"stale"'}).status_code, 412)
        result = self.client.patch('/conditional', headers={'If-Match': '"original"'})
        self.assertEqual(result.status_code, 200)
        self.assertEqual(result.headers['ETag'], '"updated"')


class QuietRequestHandler(WSGIRequestHandler):
    def log(self, type, message, *args):
        pass


class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.profile = Path(cls.temporary.name) / 'profile'
        shutil.copytree(PROFILE, cls.profile)
        cls.app = load_app(cls.profile)
        # Bind port zero atomically. No fixed ports, Unix sockets, external
        # network services, root privileges, or healthcheck/bind race.
        cls.server = make_server('127.0.0.1', 0, cls.app,
                                 threaded=True, request_handler=QuietRequestHandler)
        cls.addClassCleanup(cls.server.server_close)
        cls.thread = threading.Thread(target=cls.server.serve_forever,
                                      kwargs={'poll_interval': 0.01}, daemon=True)
        cls.thread.start()
        cls.addClassCleanup(cls.stop_server)

    @classmethod
    def stop_server(cls):
        cls.server.shutdown()
        cls.thread.join(timeout=5)
        if cls.thread.is_alive():
            raise RuntimeError('test server did not stop')

    def request(self, method, path, data=None, headers=None):
        headers = dict(headers or {})
        body = None
        if data is not None:
            headers['Content-Type'] = 'application/json'
            body = json.dumps(data).encode('utf-8')
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=5)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_server_is_ipv4_loopback_on_ephemeral_port(self):
        self.assertEqual(self.server.socket.family, socket.AF_INET)
        self.assertEqual(self.server.socket.getsockname()[0], '127.0.0.1')
        self.assertGreater(self.server.server_port, 0)

    def test_unauthenticated_service_root_and_versions(self):
        status, headers, body = self.request('GET', '/redfish')
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)['v1'], '/redfish/v1/')
        self.assertEqual(self.request('GET', json.loads(body)['v1'])[0], 200)
        self.assertEqual(self.request('GET', '/redfish/')[0], 200)
        status, headers, body = self.request('GET', '/redfish/v1')
        self.assertEqual(status, 200)
        root = json.loads(body)
        self.assertEqual(root['Id'], 'RootService')
        self.assertEqual(root['Systems']['@odata.id'], '/redfish/v1/Systems')
        self.assertIn('application/json', headers['Content-Type'])
        self.assertRegex(headers['ETag'], r'^W/"[0-9a-f]{32}"$')

    def test_basic_auth_success_missing_and_invalid(self):
        for headers, expected in [(None, 401), ({'Authorization': 'Basic invalid'}, 401), (BASIC, 200)]:
            with self.subTest(expected=expected, authorized=headers is BASIC):
                status, response_headers, _ = self.request('GET', SYSTEM, headers=headers)
                self.assertEqual(status, expected)
                if expected == 401:
                    self.assertIn('Basic', response_headers['WWW-Authenticate'])

    def test_fixed_token_success_and_invalid(self):
        self.assertEqual(self.request('GET', SYSTEM, headers=TOKEN)[0], 200)
        self.assertEqual(self.request('GET', SYSTEM, headers={'X-Auth-Token': 'invalid'})[0], 401)

    def test_inventory_collections_and_resources(self):
        for collection in ('Systems', 'Chassis', 'Managers'):
            with self.subTest(collection=collection):
                status, _, body = self.request('GET', '/redfish/v1/' + collection, headers=BASIC)
                self.assertEqual(status, 200)
                resources = json.loads(body)
                self.assertEqual(resources['Members@odata.count'], len(resources['Members']))
                for member in resources['Members']:
                    status, _, body = self.request('GET', member['@odata.id'], headers=BASIC)
                    self.assertEqual(status, 200)
                    self.assertEqual(json.loads(body)['@odata.id'], member['@odata.id'])

    def test_metadata_and_odata_are_readable(self):
        status, headers, body = self.request('GET', '/redfish/v1/$metadata')
        self.assertEqual(status, 200)
        self.assertIn('application/xml', headers['Content-Type'])
        self.assertIn(b'<?xml', body)
        self.assertEqual(self.request('GET', '/redfish/v1/odata')[0], 200)

    def test_session_login_and_logout_return_legacy_responses(self):
        uri = '/redfish/v1/SessionService/Sessions'
        self.assertEqual(self.request('POST', uri, {'UserName': 'root', 'Password': 'wrong'})[0], 401)
        status, headers, body = self.request('POST', uri, {'UserName': 'root', 'Password': 'password123456'})
        self.assertEqual(status, 201)
        self.assertEqual(headers['X-Auth-Token'], TOKEN['X-Auth-Token'])
        self.assertEqual(json.loads(body)['Id'], 'SESSION123456')
        self.assertEqual(self.request('DELETE', headers['Location'], headers=TOKEN)[0], 204)
        # Document the fixed-token simulator limitation: logout does not revoke it.
        self.assertEqual(self.request('GET', SYSTEM, headers=TOKEN)[0], 200)

    def test_authentication_handlers_do_not_log_credentials(self):
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            self.assertEqual(self.request('GET', SYSTEM, headers=BASIC)[0], 200)
            self.assertEqual(self.request('POST', '/redfish/v1/SessionService/Sessions',
                                         {'UserName': 'root', 'Password': 'do-not-log-test-password'})[0], 401)
        self.assertNotIn(BASIC['Authorization'], captured.getvalue())
        self.assertNotIn('do-not-log-test-password', captured.getvalue())

    def test_system_patch_changes_etag_and_stays_in_memory(self):
        fixture = self.profile / SYSTEM.lstrip('/') / 'index.json'
        before_disk = fixture.read_bytes()
        status, headers, body = self.request('GET', SYSTEM, headers=BASIC)
        self.assertEqual(status, 200)
        original = json.loads(body)['AssetTag']
        original_etag = headers['ETag']
        try:
            status, headers, body = self.request('PATCH', SYSTEM, {'AssetTag': 'LINUX-ISOLATED-TEST'}, BASIC)
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(body)['AssetTag'], 'LINUX-ISOLATED-TEST')
            self.assertNotEqual(headers['ETag'], original_etag)
            status, read_headers, body = self.request('GET', SYSTEM, headers=BASIC)
            self.assertEqual(status, 200)
            self.assertEqual(read_headers['ETag'], headers['ETag'])
            self.assertEqual(json.loads(body)['AssetTag'], 'LINUX-ISOLATED-TEST')
            self.assertEqual(fixture.read_bytes(), before_disk)
        finally:
            self.request('PATCH', SYSTEM, {'AssetTag': original}, BASIC)

    def test_patch_auth_and_invalid_boot_value(self):
        self.assertEqual(self.request('PATCH', SYSTEM, {'AssetTag': 'unauthorized'})[0], 401)
        self.assertEqual(self.request('PATCH', SYSTEM,
                                     {'Boot': {'BootSourceOverrideTarget': 'not-a-device'}}, BASIC)[0], 400)

    def test_existing_resource_etags_are_headers_only(self):
        # Explicitly characterize the pre-existing gap. Do not mistake these
        # responses for successful optimistic concurrency or cache validation.
        status, headers, _ = self.request('GET', SYSTEM, headers=BASIC)
        self.assertEqual(status, 200)
        conditional = dict(BASIC, **{'If-None-Match': headers['ETag']})
        self.assertEqual(self.request('GET', SYSTEM, headers=conditional)[0], 200)
        conditional = dict(BASIC, **{'If-Match': '"stale-etag"'})
        self.assertEqual(self.request('PATCH', SYSTEM, {}, conditional)[0], 200)

    def test_reset_action_is_simulated(self):
        uri = SYSTEM + '/Actions/ComputerSystem.Reset'
        try:
            self.assertEqual(self.request('POST', uri, {'ResetType': 'ForceOff'}, BASIC)[0], 204)
            self.assertEqual(json.loads(self.request('GET', SYSTEM, headers=BASIC)[2])['PowerState'], 'Off')
            self.assertEqual(self.request('POST', uri, {'ResetType': 'invalid'}, BASIC)[0], 400)
        finally:
            self.request('POST', uri, {'ResetType': 'On'}, BASIC)


class TlsTests(unittest.TestCase):
    def test_real_https_on_ephemeral_loopback_port(self):
        # cryptography is already a dependency of requirements.txt's pyOpenSSL.
        import datetime
        import ipaddress
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.x509.oid import NameOID

        with tempfile.TemporaryDirectory() as directory:
            key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
            name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'simulator-test')])
            now = datetime.datetime.now(datetime.timezone.utc)
            cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                    .public_key(key.public_key()).serial_number(x509.random_serial_number())
                    .not_valid_before(now - datetime.timedelta(minutes=1))
                    .not_valid_after(now + datetime.timedelta(days=1))
                    .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]),
                                   critical=False).sign(key, hashes.SHA256()))
            cert_path = Path(directory) / 'certificate.pem'
            key_path = Path(directory) / 'key.pem'
            cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
            key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                                  serialization.PrivateFormat.PKCS8,
                                                  serialization.NoEncryption()))
            # A temporary test key only. Never load or transmit user credentials.
            if os.name != 'nt':
                key_path.chmod(0o600)
            app = load_app()
            server = make_server('127.0.0.1', 0, app, request_handler=QuietRequestHandler,
                                 ssl_context=(str(cert_path), str(key_path)))
            thread = threading.Thread(target=server.serve_forever,
                                      kwargs={'poll_interval': 0.01}, daemon=True)
            thread.start()
            connection = http.client.HTTPSConnection('127.0.0.1', server.server_port, timeout=5,
                                                     context=ssl.create_default_context(cafile=str(cert_path)))
            try:
                self.assertEqual(server.socket.getsockname()[0], '127.0.0.1')
                self.assertNotEqual(server.server_port, 443)
                connection.request('GET', '/redfish/v1')
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(json.loads(response.read())['Id'], 'RootService')
            finally:
                connection.close()
                server.shutdown()
                thread.join(timeout=5)
                server.server_close()
                self.assertFalse(thread.is_alive())


if __name__ == '__main__':
    unittest.main()
