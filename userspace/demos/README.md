# userspace/demos — demonstrations

Programs whose purpose is to show that something works, to a person watching.
They ship into `/demos`, except the three package archives (`matrix`, `life`,
`fetch`), which ship into `/pkg` as `.pkg` files for `apm` to install.

A demo is not a test. The distinction is what happens when it goes wrong: a
test **reports** a failure in a form a script can check, and a demo simply
looks wrong to whoever is watching. If you find yourself adding assertions,
the program belongs in `../tests`.

`glcube` and `glgears` link against libgl and are the visual counterparts to
`../tests/gltest`, which checks the same code without anyone having to look
at it.
