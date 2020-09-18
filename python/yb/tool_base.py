# Copyright (c) Yugabyte, Inc.

import argparse
import os

from overrides import overrides, EnforceOverrides


class YbBuildToolBase(EnforceOverrides):
    """
    A base class for command-line tools that are part of YugabyteDB build.
    """

    def get_description(self):
        raise NotImplementedError()

    def get_arg_parser_kwargs(self):
        return dict(description=self.get_description())

    def __init__(self):
        self.arg_parser = None
        self.args = None

        # Whether to add "standard" arguments needed by most build tools.
        self.add_standard_build_args = True

    def run(self):
        """
        The top-level function used to run the tool.
        """
        self.create_arg_parser()
        self.parse_args()
        self.validate_and_process_args()
        self.run_impl()

    def parse_args(self):
        self.args = self.arg_parser.parse_args()

    def validate_and_process_args(self):
        if hasattr(self.args, 'build_root'):
            if self.args.build_root is None:
                raise ValueError('--build_root (or BUILD_ROOT environment variable) not specified')
            build_root_from_env = os.getenv('BUILD_ROOT')
            if build_root_from_env is not None and build_root_from_env != self.args.build_root:
                raise ValueError(
                    "The BUILD_ROOT environment variable is %s but the --build_root option "
                    "was specified as %s" % (build_root_from_env, self.args.build_root))
            os.environ['BUILD_ROOT'] = self.args.build_root

    def run_impl(self):
        """
        The overridable internal implementation of running the tool.
        """
        raise NotImplementedError()

    def add_command_line_args(self):
        """
        Can be overridden to add more command-line arguments to the parser.
        """
        pass

    def create_arg_parser(self):
        # Don't allow to run this function multiple times.
        assert self.arg_parser is None

        self.arg_parser = argparse.ArgumentParser(**self.get_arg_parser_kwargs())
        if self.add_standard_build_args:
            self.add_build_root_arg()
            self.add_compiler_type_arg()
            self.add_thirdparty_dir_arg()

        self.add_command_line_args()

    # ---------------------------------------------------------------------------------------------
    # Functions to add various standard arguments
    # ---------------------------------------------------------------------------------------------

    def add_build_root_arg(self):
        self.arg_parser.add_argument(
            '--build_root',
            default=os.environ.get('BUILD_ROOT'),
            help='YugabyteDB build root directory')

    def add_compiler_type_arg(self):
        self.arg_parser.add_argument(
            '--compiler_type',
            default=os.getenv('YB_COMPILER_TYPE'),
            help='Compiler type, e.g. gcc or clang')

    def add_thirdparty_dir_arg(self):
        self.arg_parser.add_argument(
            '--thirdparty_dir',
            default=os.getenv('YB_THIRDPARTY_DIR'),
            help='YugabyteDB third-party dependencies directory')
