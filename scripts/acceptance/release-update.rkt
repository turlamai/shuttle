#lang racket/base

;; Release-kind acceptance runner: runs the release-update suite —
;; HTTPS transport gates, signed-feed rejection, rollout/semver gates,
;; the N→N+1 lifecycle with data preservation, sha-mismatch landing
;; hygiene, and install-flavor artifact selection — and archives
;; evidence under .rivet/acceptance/ on success, the executable record
;; the acceptance contract requires for `release` suites. The release
;; pipeline binds this step before signing, so a release cannot ship
;; without its update evidence.

(require json
         racket/file
         racket/path
         racket/system)

(define platform
  (case (system-type 'os)
    [(windows) "windows"]
    [(macosx) "macos"]
    [(unix) "linux"]
    [else (error 'shuttle-acceptance "unsupported platform")]))

(define raco
  (or (find-executable-path "raco")
      (error 'shuttle-acceptance "raco was not found")))

(unless (system* raco "test" "tests/release-update-test.rkt")
  (error 'shuttle-acceptance "release-update suite failed"))

;; The evidence path is the one declared in rivet-acceptance.rktd —
;; each platform's acceptance run executes on its own runner, so the
;; single path carries that run's record.
(define evidence-directory
  (build-path (current-directory) ".rivet" "acceptance"))
(make-directory* evidence-directory)
(call-with-output-file (build-path evidence-directory "release-update.json")
  #:exists 'truncate/replace
  (lambda (output)
    (write-json
     (hash 'platform platform
           'release-update-verified #t
           'source "raco test tests/release-update-test.rkt")
     output)
    (newline output)))
