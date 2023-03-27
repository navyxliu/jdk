#!/bin/bash
java -XX:+UnlockExperimentalVMOptions -XX:CompileCommand=quiet -XX:CompileCommand='compileOnly,MegaMerger::*' -XX:-UseOnStackReplacement -XX:-TieredCompilation -XX:-UseTLAB -XX:CompileCommand=dontinline,MegaMerger::blackhole -XX:CompileCommand=inline,MegaMerge::foo $* MegaMerger
