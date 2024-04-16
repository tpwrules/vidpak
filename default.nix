{ lib, buildPythonPackage, cython, numpy }:

buildPythonPackage rec {
  pname = "vidpak";
  version = "0.4.1";

  # TODO: figure nicer ways to not have to specify each dir?
  src = lib.sourceByRegex ./. [
    "^vidpak$"
    "^FiniteStateEntropy$"
    "^FiniteStateEntropy/lib$"
    ".*\.pyx?$"
    ".*\.[ch]$"
  ];

  buildInputs = [ cython ];
  propagatedBuildInputs = [ numpy ];

  pythonImportsCheck = [ "vidpak" ];

  doCheck = false;
}
