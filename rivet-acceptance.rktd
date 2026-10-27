#hasheq(
 (version . 1)
 (requirements
  . (#hasheq((kind . contract) (platforms . (windows macos linux)))
     #hasheq((kind . native-build) (platforms . (windows macos linux)))))
 (suites
  . (#hasheq((id . schema-contract)
             (kind . contract)
             (platforms . (windows macos linux))
             (command . ("raco" "rivet" "schema" "check" "rivet-schema.json" "--json"))
             (timeout-seconds . 120)
             (evidence . ()))
     #hasheq((id . native-build)
             (kind . native-build)
             (platforms . (windows macos linux))
             (command . ("raco" "rivet" "build"))
             (timeout-seconds . 1800)
             (evidence . ())))))