# Security

vkdeflate decodes compressed streams on the CPU and the GPU, often from files a game did not make itself. A crash,
a hang or a write outside the output on a broken stream is a security bug.

Report it privately with **Report a vulnerability** on the Security tab of the repository, not in a public issue.
Send the stream or how to make it, the GPU and the driver, and what happened.

Fixes go into the latest release.

With `config_t::bounds_checks` turned off, a broken stream can write past its tile or never finish. That is not a
bug.
