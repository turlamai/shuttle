#lang racket/base

;; Release-kind acceptance: the update chain evidence every Shuttle
;; release must ship with (the taskly UPDATE.md family contract, slice
;; for shuttle's windows-only MSI + portable shapes). Driven end to end
;; at the rivet/distribution boundary the backend's ensure-updater!
;; configures — same application identity, same key-id vocabulary, same
;; managed-updater state machine the host polls.
;;
;; Rivet's manifest fetch hard-requires HTTPS (the production feed is
;; HTTPS-only), so the suite splits in two: a plain-http feed must never
;; yield a candidate through the real updater-check!, and the full
;; N→N+1 lifecycle runs the identical verify→select→download pipeline
;; over a loopback feed signed with a throwaway key, with the manifest
;; transport swapped through current-managed-fetch-manifest and the
;; artifact re-targeted through current-managed-download. Cancel, stall
;; and watchdog behavior live inside rivet's managed-updater and are
;; covered by rivet's own suite; what shuttle must prove on every
;; release is selection, verification, landing hygiene and that user
;; data (config.json, history.jsonl) is never touched.

(require crypto
         crypto/all
         racket/file
         racket/list
         racket/path
         racket/port
         racket/tcp
         rackunit
         (only-in rivet/distribution
                  write-signed-manifest
                  verify-signed-manifest
                  fetch-update-manifest
                  download-update
                  update-manifest
                  update-manifest-version
                  update-artifact
                  update-artifact-installer
                  update-artifact-url
                  update-candidate
                  update-candidate-manifest
                  update-candidate-artifact
                  updater-config
                  select-update
                  sha256-file/hex
                  make-updater
                  updater-check!
                  updater-download!
                  updater-wait
                  updater-state)
         ;; The fetch/download seams are test-support surface: present in
         ;; managed-updater but exported only from its test submod (the
         ;; same import rivet's own managed-updater suite uses).
         (submod rivet/distribution/managed-updater test-support)
         net/url)

(use-all-factories!)

;; The identity constants mirror app/backend.rkt's updater construction;
;; the release preflight pins the backend side, this suite pins the
;; boundary the manifest feed actually serves.
(define shuttle-id "dev.rivet.shuttle")
(define shuttle-key-id "shuttle-test")
(define current-shuttle-version "1.2.2")

;; ---------- fixtures ------------------------------------------------------

(define (sha256-bytes/hex bytes)
  (define scratch (make-temporary-file "shuttle-release-sha-~a"))
  (dynamic-wind
    (lambda ()
      (call-with-output-file scratch #:exists 'truncate
        (lambda (out) (write-bytes bytes out))))
    (lambda () (sha256-file/hex scratch))
    (lambda () (delete-file scratch))))

(define (wait-until ready? who [timeout-ms 15000])
  (let loop ([deadline (+ (current-inexact-milliseconds) timeout-ms)])
    (cond
      [(ready?) (void)]
      [(> (current-inexact-milliseconds) deadline)
       (fail (string-append "timed out waiting for " who))]
      [else (sleep 0.05) (loop deadline)])))

;; Loopback feed: reserve a port first (manifest URLs must embed it),
;; then serve an assoc list of (path-substring . bytes) over HTTP — the
;; same transport rivet's own download tests use.
(define (open-loopback)
  (tcp-listen 0 16 #f))

(define (loopback-port listener)
  (cadr (call-with-values (lambda () (tcp-addresses listener #t)) list)))

(define (serve-on! listener routes)
  (thread
   (lambda ()
     (let accept ()
       (define-values (cin cout) (tcp-accept listener))
       (thread
        (lambda ()
          (with-handlers ([exn:fail? (lambda (_) (void))])
            (define request-line (read-line cin 'return-linefeed))
            (let drain ()
              (define line (read-line cin 'return-linefeed))
              (unless (or (eof-object? line) (equal? line "")) (drain)))
            (define route
              (for/first ([pair (in-list routes)]
                          #:when (regexp-match? (regexp-quote (car pair))
                                                request-line))
                pair))
            (cond
              [route
               (define body (cdr route))
               (display "HTTP/1.1 200 OK\r\n" cout)
               (display (format "Content-Length: ~a\r\n" (bytes-length body))
                        cout)
               (display "Connection: close\r\n\r\n" cout)
               (write-bytes body cout)]
              [else
               (display
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                cout)])
            (flush-output cout)
            ;; Close the output half for EOF: closing the input half
            ;; first deadlocks both peers (close-input-port waits).
            (close-output-port cout))))
       (accept)))))

;; A throwaway Ed25519 identity: the private half signs the feed, the
;; public half plays the role the embedded production key plays live.
(define (test-signing-key)
  (define priv (generate-private-key 'eddsa '((curve ed25519))))
  (values priv
          (datum->pk-key (pk-key->datum priv 'rkt-public) 'rkt-public)))

(define (wrapped-feed manifest priv)
  (define wrapped (open-output-bytes))
  (write-signed-manifest manifest priv shuttle-key-id wrapped)
  (get-output-bytes wrapped))

(define (check-config key version)
  (updater-config shuttle-id version 'stable 'windows 'x64
                  key shuttle-key-id 0 (* 100 1024 1024) #f))

(define (msi-artifact url bytes)
  (update-artifact 'windows 'x64 url (sha256-bytes/hex bytes)
                   (bytes-length bytes) 'msi '() #f))

(define (zip-artifact url bytes)
  (update-artifact 'windows 'x64 url (sha256-bytes/hex bytes)
                   (bytes-length bytes) 'zip '() #f))

;; Shuttle's user data: config.json + history.jsonl live beside the
;; updater's data directory — the thing an update must never touch.
(define (seeded-home)
  (define home (make-temporary-file "shuttle-release-home-~a" 'directory))
  (call-with-output-file (build-path home "config.json") #:exists 'truncate
    (lambda (out) (display "{\"identity\":\"OFFICE-PC-A\"}" out)))
  (call-with-output-file (build-path home "history.jsonl") #:exists 'truncate
    (lambda (out) (displayln "{\"ok\":true}" out)))
  home)

(define (data-shas home)
  (map sha256-file/hex
       (list (build-path home "config.json")
             (build-path home "history.jsonl"))))

;; The fetch seam the lifecycle arm swaps in: identical to rivet's real
;; fetch-update-manifest minus the HTTPS transport gate — envelope over
;; the (loopback) wire, then the production verify path.
(define (loopback-fetch in)
  (lambda (url key #:key-id key-id
               #:trusted-keys [trusted-keys #f]
               #:maximum-bytes [maximum-bytes #f])
    (define port (open-input-bytes in))
    (verify-signed-manifest port key #:key-id key-id)))

;; ---------- arms ----------------------------------------------------------

(test-case "the manifest transport gate is HTTPS-only"
  ;; Two layers back the production feed's HTTPS-only contract: the
  ;; updater constructor refuses a plain-http manifest URL outright, and
  ;; the low-level fetch fails closed on one too. A loopback http feed
  ;; can therefore never yield a candidate through any shuttle path.
  (define-values (_priv pub) (test-signing-key))
  (check-exn exn:fail?
             (lambda ()
               (make-updater
                #:application-id shuttle-id
                #:current-version current-shuttle-version
                #:channel 'stable
                #:platform 'windows
                #:architecture 'x64
                #:install-kind 'msi
                #:public-key pub
                #:key-id shuttle-key-id
                #:manifest-url "http://127.0.0.1:1/update-stable.json"
                #:data-directory (make-temporary-file
                                  "shuttle-release-gate-~a" 'directory)
                #:rollout-bucket 41)))
  (check-exn exn:fail?
             (lambda ()
               (fetch-update-manifest
                "http://127.0.0.1:1/update-stable.json" pub
                #:key-id shuttle-key-id))))

(test-case "a re-keyed or re-signed feed fails closed"
  (define-values (priv-a pub-a) (test-signing-key))
  (define-values (priv-b pub-b) (test-signing-key))
  (define manifest
    (update-manifest shuttle-id "9.9.9" 8 'stable
                     "2026-11-08T00:00:00Z" "0.0.0" #f #t 100
                     (list (msi-artifact "https://dl.example/shuttle-9.9.9.msi"
                                         (make-bytes 8 2)))))
  ;; The exact bytes, under their own key-id: the happy path every client
  ;; runs against the production feed.
  (check-not-exn
   (lambda ()
     (verify-signed-manifest (open-input-bytes (wrapped-feed manifest priv-a))
                             pub-a
                             #:key-id shuttle-key-id)))
  ;; A different key-id must fail closed (the shipped clients pin ours).
  (check-exn exn:fail?
             (lambda ()
               (verify-signed-manifest
                (open-input-bytes (wrapped-feed manifest priv-a))
                pub-a
                #:key-id "some-other-key")))
  ;; The same key-id under a different trusted key (a re-signed feed from
  ;; a key rotation the client never adopted) must also fail closed.
  (check-exn exn:fail?
             (lambda ()
               (verify-signed-manifest
                (open-input-bytes (wrapped-feed manifest priv-b))
                pub-a
                #:key-id shuttle-key-id))))

(test-case "rollout 0 gates the offer and downgrades are refused"
  (define-values (priv pub) (test-signing-key))
  (define gated
    (update-manifest shuttle-id "9.9.9" 8 'stable
                     "2026-11-08T00:00:00Z" "0.0.0" #f #t 0
                     (list (msi-artifact "https://dl.example/shuttle-9.9.9.msi"
                                         (make-bytes 8 3)))))
  (check-false (select-update (check-config pub current-shuttle-version)
                              gated)
               "rollout 0 must gate the offer for every bucket")
  (define downgrade
    (update-manifest shuttle-id "1.2.1" 7 'stable
                     "2026-11-05T00:00:00Z" "0.0.0" #f #t 100
                     (list (msi-artifact "https://dl.example/shuttle-1.2.1.msi"
                                         (make-bytes 8 4)))))
  (check-false (select-update (check-config pub current-shuttle-version)
                              downgrade)
               "a downgrade offer violates the semver gate"))

(test-case "the N→N+1 lifecycle preserves user data byte for byte"
  (define home (seeded-home))
  (define before (data-shas home))
  (define artifact-bytes (make-bytes 4096 7))
  (define-values (priv pub) (test-signing-key))
  (define listener (open-loopback))
  (define base (format "http://127.0.0.1:~a" (loopback-port listener)))
  ;; The signed envelope carries https URLs — the sign/parse layers
  ;; reject anything else (transport gate) — and selection never fetches.
  ;; The download leg runs on the same artifact re-targeted at the
  ;; loopback server through the current-managed-download seam.
  (define manifest
    (update-manifest shuttle-id "9.9.9" 8 'stable
                     "2026-11-08T00:00:00Z" "0.0.0" #f #t 100
                     (list (msi-artifact
                            "https://dl.example/shuttle-9.9.9-windows-x64.msi"
                            artifact-bytes))))
  (define server-thread
    (serve-on! listener
               (list (cons "update-stable.json" (wrapped-feed manifest priv))
                     (cons "shuttle-9.9.9-windows-x64.msi" artifact-bytes))))
  (define envelope (wrapped-feed manifest priv))
  (parameterize ([current-managed-fetch-manifest (loopback-fetch envelope)]
                 [current-managed-download
                  (lambda (config candidate destination
                                  #:on-progress [on-progress void]
                                  #:progress-every [every #f])
                    (define loopback-artifact
                      (msi-artifact
                       (string-append
                        base "/shuttle-9.9.9-windows-x64.msi")
                       artifact-bytes))
                    (define re-targeted
                      (update-candidate
                       (struct-copy
                        update-manifest
                        (update-candidate-manifest candidate)
                        [artifacts (list loopback-artifact)])
                       loopback-artifact))
                    (download-update config re-targeted destination
                                     #:on-progress on-progress
                                     #:progress-every every))])
    (define u
      (make-updater
       #:application-id shuttle-id
       #:current-version current-shuttle-version
       #:channel 'stable
       #:platform 'windows
       #:architecture 'x64
       #:install-kind 'msi
       #:public-key pub
       #:key-id shuttle-key-id
       #:manifest-url "https://github.com/turlamai/shuttle/releases/latest/download/update-stable.json"
       #:data-directory (build-path home "updates")
       #:rollout-bucket 41))
    (define result (updater-check! u))
    (check-equal? (hash-ref result 'status) "available")
    (check-equal? (hash-ref result 'availableVersion) "9.9.9")
    (check-equal? (hash-ref (updater-state u) 'phase) "available")
    (updater-download! u)
    (wait-until
     (lambda ()
       (let ([phase (hash-ref (updater-state u) 'phase)])
         (member phase '("downloaded" "error" "cancelled"))))
     "the downloaded phase")
    (check-equal? (hash-ref (updater-state u) 'phase) "downloaded")
    (define downloaded (hash-ref (updater-state u) 'downloadedPath))
    (check-true (and downloaded (file-exists? downloaded)))
    (check-equal? (sha256-file/hex downloaded)
                  (sha256-bytes/hex artifact-bytes))
    (check-false
     (file-exists? (path-add-extension downloaded #".partial"))
     "the .partial must be renamed away, not left behind")
    (check-equal? (data-shas home) before
                  "the update flow must not touch user data"))
  (kill-thread server-thread)
  (tcp-close listener))

(test-case "a sha-mismatched artifact refuses to land and allows retry"
  (define home (seeded-home))
  (define before (data-shas home))
  (define-values (priv pub) (test-signing-key))
  (define listener (open-loopback))
  (define base (format "http://127.0.0.1:~a" (loopback-port listener)))
  ;; The manifest vouches for zeros; the server sends nines — the size
  ;; matches, the hash cannot.
  (define manifest
    (update-manifest shuttle-id "9.9.9" 8 'stable
                     "2026-11-08T00:00:00Z" "0.0.0" #f #t 100
                     (list (msi-artifact
                            "https://dl.example/shuttle-9.9.9-windows-x64.msi"
                            (make-bytes 512 0)))))
  (define server-thread
    (serve-on! listener
               (list (cons "update-stable.json" (wrapped-feed manifest priv))
                     (cons "shuttle-9.9.9-windows-x64.msi"
                           (make-bytes 512 9)))))
  (define envelope (wrapped-feed manifest priv))
  (parameterize ([current-managed-fetch-manifest (loopback-fetch envelope)]
                 [current-managed-download
                  (lambda (config candidate destination
                                  #:on-progress [on-progress void]
                                  #:progress-every [every #f])
                    (define loopback-artifact
                      (msi-artifact
                       (string-append
                        base "/shuttle-9.9.9-windows-x64.msi")
                       (make-bytes 512 0)))
                    (define re-targeted
                      (update-candidate
                       (struct-copy
                        update-manifest
                        (update-candidate-manifest candidate)
                        [artifacts (list loopback-artifact)])
                       loopback-artifact))
                    (download-update config re-targeted destination
                                     #:on-progress on-progress
                                     #:progress-every every))])
    (define u
      (make-updater
       #:application-id shuttle-id
       #:current-version current-shuttle-version
       #:channel 'stable
       #:platform 'windows
       #:architecture 'x64
       #:install-kind 'msi
       #:public-key pub
       #:key-id shuttle-key-id
       #:manifest-url "https://github.com/turlamai/shuttle/releases/latest/download/update-stable.json"
       #:data-directory (build-path home "updates")
       #:rollout-bucket 41))
    (updater-check! u)
    (updater-download! u)
    (wait-until
     (lambda ()
       (let ([phase (hash-ref (updater-state u) 'phase)])
         (member phase '("error" "cancelled"))))
     "the sha-mismatch error phase")
    (check-equal? (hash-ref (updater-state u) 'phase) "error")
    (define destination
      (build-path home "updates"
                  "dev.rivet.shuttle-9.9.9-windows-x64.msi"))
    (check-false (file-exists? destination)
                 "a rejected artifact must never reach the destination")
    (check-false
     (file-exists? (path-add-extension destination #".partial")))
    ;; The failed worker released its operation slot: a retry must start
    ;; instead of dying on "another updater operation is already running".
    (check-not-exn (lambda () (updater-download! u)))
    (check-equal? (data-shas home) before))
  (kill-thread server-thread)
  (tcp-close listener))

(test-case "the install flavor selects the matching signed artifact"
  ;; The host stamps install-flavor before every check (UPDATE.md
  ;; ordering rule): an MSI install must be offered the MSI, a portable
  ;; copy the zip — both from the same signed manifest.
  (define-values (priv pub) (test-signing-key))
  (define msi-bytes (make-bytes 32 1))
  (define zip-bytes (make-bytes 32 2))
  (define manifest
    (update-manifest shuttle-id "9.9.9" 8 'stable
                     "2026-11-08T00:00:00Z" "0.0.0" #f #t 100
                     (list (msi-artifact "https://dl.example/shuttle-9.9.9.msi"
                                         msi-bytes)
                           (zip-artifact "https://dl.example/shuttle-9.9.9.zip"
                                         zip-bytes))))
  (define msi-candidate
    (select-update (check-config pub current-shuttle-version) manifest
                   #:install-kind 'msi))
  (check-true (and msi-candidate #t) "an MSI install must be offered the update")
  (check-equal? (update-artifact-installer
                 (update-candidate-artifact msi-candidate))
                'msi)
  (define portable-candidate
    (select-update (check-config pub current-shuttle-version) manifest
                   #:install-kind 'portable))
  (check-true (and portable-candidate #t)
              "a portable install must be offered the update")
  (check-equal? (update-artifact-installer
                 (update-candidate-artifact portable-candidate))
                'zip)
  (check-equal? (update-artifact-url (update-candidate-artifact portable-candidate))
                "https://dl.example/shuttle-9.9.9.zip"
                "the portable offer must carry the zip, not the msi"))
