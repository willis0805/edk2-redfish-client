# Copyright Notice:
#
# Copyright (c) 2019, Intel Corporation. All rights reserved.<BR>
# Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# Copyright Notice:
# Copyright 2016 Distributed Management Task Force, Inc. All rights reserved.
# License: BSD 3-Clause License. For full text see link: https://github.com/DMTF/Redfish-Profile-Simulator/blob/master/LICENSE.md

# This program is dependent on the following Python packages that should be installed separately with pip:
#    pip install Flask
#
# standard python packages
import sys
import getopt
import os
import functools
import flask
import werkzeug

rfVersion = "0.9.6"
rfProgram1 = "redfishProfileSimulator"
rfProgram2 = "                "
rfUsage1 = "[-Vh]  [--Version][--help]"
rfUsage2 = "[-H<hostIP>] [-P<port>] [-C<cert>] [-K<key>] [-p<profile_path>]"
rfUsage3 = "[--Host=<hostIP>] [--Port=<port>] [--Cert=<cert>] [--Key=<key>] [--profile=<profile_path>]"


def rf_usage():
        print("Usage:")
        print("  ", rfProgram1, "  ", rfUsage1)
        print("  ", rfProgram1, "  ", rfUsage2)
        print("  ", rfProgram2, "  ", rfUsage3)


def rf_help():
        print(rfProgram1,"implements a simulation of a redfish service for the \"Simple OCP Server V1\" Mockup.")
        print(" The simulation includes an http/https server, RestEngine, and dynamic Redfish datamodel.")
        print(" You can GET, PATCH,... to the service just like a real Redfish service.")
        print(" Both Basic and Redfish Session/Token authentication is supported (for a single user/passwd and token")
        print("    Basic auth: admin/pwd123456. Session login: root/password123456.")
        print("    The fixed simulation authToken is: 123456SESSIONauthcode")
        print("    these can be changed by editing the redfishURIs.py file.  will make dynamic later.")
        print(" The http/https service and Rest engine is built on Flask. Use a current Python 3 release supported by requirements.txt.")
        print(" The data model resources are \"initialized\" from the SPMF \"SimpleOcpServerV1\" Mockup.")
        print("     and stored as python dictionaries--then the dictionaries are updated with patches, posts, deletes.")
        print(" The program can be extended to support other mockup \"profiles\".")
        print("")
        print(" By default, the simulation runs over http, on localhost (127.0.0.1), on port 5000.")
        print(" These can be changed with CLI options: -P<port> -C<cert> -K<key> -H <hostIP> | --port=<port> --Cert=<cert> --Key=<key> --host=<hostIp>")
        print(" Supply both -C<cert> and -K<key> to enable HTTPS on any port (for example 8443).")
        print("")
        print("Version: ", rfVersion)
        rf_usage()
        print("")
        print("       -V,          --Version,                       --- the program version")
        print("       -h,          --help,                          --- help")
        print("       -H<hostIP>,  --Host=<hostIp>                  --- host IP address. dflt=127.0.0.1")
        print("       -P<port>,    --Port=<port>                    --- the port to use. dflt=5000")
        print("       -C<cert>,    --Cert=<cert>                    --- Server certificate.")
        print("       -K<key>,     --Key=<key>                      --- Server key.")
        print("       -p<profile_path>, --profile=<profile_path>    --- the path to the Redfish profile to use. "
              "dflt=MockupData/SimpleOcpServerV1 beside this script")

# Conditional Requests with ETags
# http://flask.pocoo.org/snippets/95/
def conditional(func):
    '''Start conditional method execution for this resource'''
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        flask.g.condtnl_etags_start = True
        return func(*args, **kwargs)
    return wrapper

class NotModified(werkzeug.exceptions.HTTPException):
    code = 304
    def get_response(self, environ=None, scope=None):
        return flask.Response(status=304)

class PreconditionRequired(werkzeug.exceptions.HTTPException):
    code = 428
    description = ('<p>This request is required to be '
                   'conditional; try using "If-Match".')
    name = 'Precondition Required'
    def get_response(self, environ=None, scope=None):
        resp = super(PreconditionRequired,
                     self).get_response(environ, scope)
        resp.status = str(self.code) + ' ' + self.name.upper()
        return resp

def install_conditional_etags():
    """Install the legacy conditional-response hook once per interpreter."""
    if getattr(werkzeug.wrappers.Response.set_etag, '_redfish_conditional', False):
        return
    _old_set_etag = werkzeug.wrappers.Response.set_etag

    @functools.wraps(_old_set_etag)
    def _new_set_etag(self, etag, weak=False):
        # Only check the first call for a decorated request. Plain Werkzeug
        # responses are also valid outside a Flask request context.
        if (flask.has_request_context() and
                getattr(flask.g, 'condtnl_etags_start', False)):
            if flask.request.method in ('PUT', 'DELETE', 'PATCH'):
                if not flask.request.if_match:
                    raise PreconditionRequired
                if etag not in flask.request.if_match:
                    flask.abort(412)
            elif (flask.request.method == 'GET' and
                  flask.request.if_none_match and
                  etag in flask.request.if_none_match):
                raise NotModified
            flask.g.condtnl_etags_start = False
        _old_set_etag(self, etag, weak)

    _new_set_etag._redfish_conditional = True
    werkzeug.wrappers.Response.set_etag = _new_set_etag


def parse_options(argv):
    """Parse legacy short/long options without depending on the working directory."""
    options = {
        'profile': os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                'MockupData', 'SimpleOcpServerV1'),
        'host': '127.0.0.1',
        'port': 5000,
        'cert': '',
        'key': '',
    }
    opts, args = getopt.getopt(
        argv[1:], "VhH:P:C:K:p:",
        ["Version", "version", "help", "Host=", "host=", "Port=", "port=",
         "Cert=", "cert=", "Key=", "key=", "profile=", "profile_path="])
    if args:
        raise ValueError("unexpected positional argument: {}".format(args[0]))
    for opt, arg in opts:
        if opt in ("-h", "--help"):
            rf_help()
            return None
        elif opt in ("-V", "--Version", "--version"):
            print("Version:", rfVersion)
            return None
        elif opt in ("-p", "--profile", "--profile_path"):
            options['profile'] = os.path.abspath(arg)
        elif opt in ("-H", "--Host", "--host"):
            if not arg.strip():
                raise ValueError("host must not be empty")
            options['host'] = arg
        elif opt in ("-P", "--Port", "--port"):
            try:
                options['port'] = int(arg)
            except ValueError:
                raise ValueError("port must be an integer between 1 and 65535")
            if not 1 <= options['port'] <= 65535:
                raise ValueError("port must be an integer between 1 and 65535")
        elif opt in ("-C", "--Cert", "--cert"):
            options['cert'] = arg
        elif opt in ("-K", "--Key", "--key"):
            options['key'] = arg

    if bool(options['cert']) != bool(options['key']):
        raise ValueError("certificate and key must be supplied together")
    if options['port'] == 443 and not options['cert']:
        raise ValueError("port 443 requires a certificate and key; use port 5000 for HTTP")
    for name in ('cert', 'key'):
        if options[name] and not os.path.isfile(options[name]):
            raise ValueError("{} file does not exist: {}".format(name, options[name]))
    return options


def main(argv):
    try:
        options = parse_options(argv)
    except (getopt.GetoptError, ValueError) as error:
        print("{}: {}".format(rfProgram1, error), file=sys.stderr)
        rf_usage()
        return 2
    if options is None:
        return 0

    rf_profile_path = options['profile']
    for rel_path in ('redfish/index.json', 'redfish/v1/index.json'):
        if not os.path.isfile(os.path.join(rf_profile_path, rel_path)):
            print("{}: invalid profile path (missing {}): {}".format(
                rfProgram1, rel_path, rf_profile_path), file=sys.stderr)
            return 2

    from v1sim.serviceVersions import RfServiceVersions
    from v1sim.serviceRoot import RfServiceRoot
    from v1sim.redfishURIs import rfApi_SimpleServer

    install_conditional_etags()
    try:
        versions = RfServiceVersions(rf_profile_path, "redfish")
        root = RfServiceRoot(rf_profile_path, os.path.join("redfish", "v1"))
    except (OSError, ValueError, KeyError, TypeError) as error:
        print("{}: unable to load profile {}: {}".format(
            rfProgram1, rf_profile_path, error), file=sys.stderr)
        return 2

    print("{} Version: {}".format(rfProgram1, rfVersion))
    print("   Starting redfishProfileSimulator at: hostIP={}, port={}".format(
        options['host'], options['port']))
    print("   Using Profile at {}".format(rf_profile_path))
    try:
        rfApi_SimpleServer(root, versions, host=options['host'],
                           port=options['port'], cert=options['cert'],
                           key=options['key'])
    except (OSError, ValueError) as error:
        print("{}: unable to start server: {}".format(rfProgram1, error),
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
