Installation
============

``msgspec`` may be installed via ``pip`` or ``conda``. Note that Python >= 3.10
is required. The basic install has no required dependencies.

**pip**

.. code-block:: shell

    pip install msgspec

**conda**

.. code-block:: shell

    conda install msgspec -c conda-forge


.. _install-without-pcre2:

Optional PCRE2 acceleration
---------------------------

The basic ``msgspec`` wheel and source distribution contain no PCRE2 code.
For optional pattern acceleration, install the separate native backend::

    python -m pip install "msgspec[pcre2]"

This extra adds ``msgspec-pcre2``, which owns a separate extension and bundles
PCRE2 sources and licenses. No system PCRE2 library is required. Ready wheels
need no compiler; where a compatible wheel is unavailable, building from source
requires a C compiler and Python development headers.

Without the backend, all patterns use Python ``re``. With it, compatible
patterns are accelerated automatically, preserving Python ``re`` semantics.
A broken or incompatible backend raises an error instead of silently disabling
acceleration.

To remove an existing backend, uninstall ``msgspec-pcre2``. Restart Python after
adding or removing the backend. Already created matchers retain their original
engine for their lifetime.


Optional Dependencies
---------------------

Depending on your platform, the base install of ``msgspec`` may not support
TOML_ or YAML_ without additional dependencies.

TOML
~~~~

The TOML_ protocol requires:

- Python < 3.11: `tomli`_ and `tomli_w`_ for reading and writing TOML.

- Python >= 3.11: `tomli_w`_ for writing TOML. Reading TOML is done using
  the standard library's `tomllib` and requires no additional dependencies.

You may either install these dependencies manually, or depend on the ``toml``
extra:

**pip**

.. code-block:: shell

    pip install "msgspec[toml]"

**conda**

.. code-block:: shell

    conda install msgspec-toml -c conda-forge

YAML
~~~~

The YAML_ protocol requires PyYAML_ on all platforms. You may either install
this dependency manually, or depend on the ``yaml`` extra:

**pip**

.. code-block:: shell

    pip install "msgspec[yaml]"

**conda**

.. code-block:: shell

    conda install msgspec-yaml -c conda-forge


Installing from GitHub
----------------------

If you want wish to use a feature that hasn't been released yet, you may
install from the `development branch on GitHub
<https://github.com/msgspec/msgspec>`__ using ``pip``:

.. code-block:: shell

    pip install git+https://github.com/msgspec/msgspec.git


.. _YAML: https://yaml.org
.. _TOML: https://toml.io/en/
.. _PyYAML: https://pyyaml.org/
.. _tomli: https://github.com/hukkin/tomli
.. _tomli_w: https://github.com/hukkin/tomli-w
