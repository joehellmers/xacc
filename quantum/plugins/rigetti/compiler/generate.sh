#!/bin/bash
set - o errexit
java -jar antlr-4.8-complete.jar -Dlanguage=Cpp -listener -o generated/ -package quil Quil.g4
