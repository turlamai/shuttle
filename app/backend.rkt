#lang racket/base

;; Shuttle backend — mailbox-style file relay between two Windows PCs
;; over the department share. All product logic lives here; the native
;; host only presents and forwards OS integration.

(require racket/format
         racket/date
         racket/file
         racket/function
         racket/list
         racket/path
         racket/string
         racket/system
         racket/async-channel
         json
         rivet/backend)

(provide start
         ;; test hooks (used by the rackunit submodule and self-tests)
         init! do-send! receive-cycle! sweep-own-folders!
         sanitize-filename unique-dest read-manifest
         this-machine snapshot-transfers begin-send!
         list-transfers-records
         ;; share-op internals (driven by the self-tests)
         self-grant-full-control! write-manifest! write-done-marker!
         ensure-wscript-js!)

;; ---------------------------------------------------------------- contract

(define-enum Direction (send receive))
(define-enum TransferStatus (queued active sent received failed))

(define-record Transfer
  ([id : String]
   [direction : Direction]
   [filename : String]
   [size : Int64]
   [progress : Int64]
   [status : TransferStatus]
   [message : String]
   [stamp : String]))

(define-record Settings
  ([relay-dir : String]
   [download-dir : String]
   [machine : String]))

(define-record RelayHealth
  ([ok : Bool]
   [message : String]))

(define-event transfer-changed : Transfer)
(define-event relay-status : RelayHealth)

;; Background threads may emit before serve-fds installs the event
;; emitter (the first poll races startup); dropping those events is
;; correct — the host replays state via list-transfers on connect.
;; Anything else still propagates.
(define (emit-safe! emit)
  (with-handlers ([exn:fail?
                   (lambda (e)
                     (unless (string-contains? (exn-message e)
                                               "no Rivet server")
                       (raise e)))])
    (emit)))

;; ---------------------------------------------------------------- state

;; Every mutable box lives behind `state-lock`; entries are immutable
;; hasheq snapshots so event emission never races a mutation.
(define state-lock (make-semaphore 1))
(define transfers-table (make-hash))     ; id -> entry hasheq
(define consumed-ids (make-hash))        ; id -> #t; ids we must not re-process
(define send-queue (make-async-channel))
(define config (make-parameter #f))      ; hasheq with path-string fields
(define config-dir-param (make-parameter #f))
(define config-relay-ok-param (make-parameter 'unknown))
(define ready? #f)

;; Identity is the machine name; tests parameterize it to play the peer.
(define this-machine
  (make-parameter (or (getenv "COMPUTERNAME") "unknown-pc")))

(define (grant-user)
  (define domain (getenv "USERDOMAIN"))
  (define user (getenv "USERNAME"))
  (if (and domain user (not (string=? domain "")) (not (string=? user "")))
      (string-append domain "\\" user)
      #f))

(define (config-dir) (config-dir-param))
(define (config-relay-ok) (config-relay-ok-param))

;; ---------------------------------------------------------------- small utils

(define (pad2 n) (~r n #:min-width 2 #:pad-string "0"))

(define (display-stamp [d (current-date)])
  (format "~a-~a~a ~a:~a:~a"
          (date-year d) (pad2 (date-month d)) (pad2 (date-day d))
          (pad2 (date-hour d)) (pad2 (date-minute d)) (pad2 (date-second d))))

(define (folder-stamp [d (current-date)])
  (format "~a~a~a-~a~a~a"
          (date-year d) (pad2 (date-month d)) (pad2 (date-day d))
          (pad2 (date-hour d)) (pad2 (date-minute d)) (pad2 (date-second d))))

(define (new-transfer-id)
  (~r (random 4294967087) #:base 16 #:min-width 8 #:pad-string "0"))

;; Strip directories and Windows-hostile characters; reject anything
;; that could escape the download directory.
(define (sanitize-filename name)
  (define base (with-handlers ([exn:fail? (const #f)])
                 (file-name-from-path (string->path (string-trim name)))))
  (and base
       (let* ([s (path->string base)]
              [cleaned
               (list->string
                (for/list ([c (in-string s)])
                  (if (or (char<? c #\space)
                          (memv c '(#\< #\> #\: #\" #\/ #\\ #\| #\? #\*)))
                      #\_ c)))]
              [trimmed (string-trim cleaned)])
         (and (not (member trimmed '("" "." "..")))
              trimmed))))

(define (split-extension name)
  ;; path-get-extension returns the extension WITH the leading dot.
  (define ext (path-get-extension (string->path name)))
  (if ext
      (values (substring name 0 (- (string-length name) (bytes-length ext)))
              (bytes->string/utf-8 ext))
      (values name "")))

;; Never overwrite: first free "name", "name (2)", "name (3)" …
(define (unique-dest dir name)
  (define candidate (build-path dir name))
  (if (not (file-exists? candidate))
      candidate
      (let-values ([(stem ext) (split-extension name)])
        (let loop ([i 2])
          (define p (build-path dir (format "~a (~a)~a" stem i ext)))
          (if (file-exists? p) (loop (add1 i)) p)))))

;; ---------------------------------------------------------------- persistence

(define (config-file) (build-path (config-dir) "config.json"))
(define (history-file) (build-path (config-dir) "history.jsonl"))
(define (consumed-file) (build-path (config-dir) "consumed.jsonl"))

(define default-relay-dir
  (string->path "Z:\\19 汽车电子事业部\\研发部\\04_Software\\LiDAR\\.shuttle"))

(define (default-download-dir)
  (define profile (or (getenv "USERPROFILE")
                      (path->string (find-system-path 'home-dir))))
  (string->path (string-append profile "\\Downloads\\Shuttle")))

(define (path->display p)
  (if (path? p) (path->string p) (format "~a" p)))

(define (load-or-create-config!)
  (define dir (config-dir))
  (make-directory* dir)
  (define cfg
    (or (with-handlers ([exn:fail? (const #f)])
          (define j (call-with-input-file (config-file) read-json))
          (and (hash? j)
               (hash-has-key? j 'relay-dir)
               (hash-has-key? j 'download-dir)
               j))
        (hasheq 'relay-dir (path->display default-relay-dir)
                'download-dir (path->display (default-download-dir)))))
  (config cfg)
  (write-config! cfg))

(define (write-config! cfg)
  (call-with-output-file (config-file) #:exists 'truncate
    (lambda (p) (write-json cfg p))))

(define (load-consumed!)
  (with-handlers ([exn:fail? void])
    (call-with-input-file (consumed-file)
      (lambda (p)
        (for ([line (in-lines p)])
          (define id (string-trim line))
          (unless (string=? id "") (hash-set! consumed-ids id #t)))))))

(define (mark-consumed! id)
  (hash-set! consumed-ids id #t)
  (with-handlers ([exn:fail? void])
    (call-with-output-file (consumed-file) #:exists 'append
      (lambda (p) (writeln id p)))))

(define (append-history! entry)
  (with-handlers ([exn:fail? void])
    (call-with-output-file (history-file) #:exists 'append
      (lambda (p) (write-json entry p) (newline p)))))

;; History rows use the LEDGER schema (file/ts/msg/ok, written by
;; append-history!), while transfer entries use the UI schema
;; (filename/stamp/message/progress/status). Loading rows verbatim made
;; every restart after a first transfer crash list-transfers (the sort
;; key reads 'stamp). Map ledger -> entry here; rows without an id are
;; skipped entirely.
(define (history->entry j)
  (define id (hash-ref j 'id #f))
  (unless (string? id)
    (error 'load-history! "ledger row without id"))
  (define direction
    (let ([d (hash-ref j 'direction "send")])
      (if (member d '("send" "receive")) (string->symbol d) 'send)))
  (define ok (hash-ref j 'ok #f))
  (define size (hash-ref j 'size 0))
  (hasheq 'id id
          'direction direction
          'filename (hash-ref j 'file "")
          'size size
          'progress size
          'status (cond [ok (if (eq? direction 'receive) 'received 'sent)]
                        [else 'failed])
          'message (hash-ref j 'msg "")
          'stamp (hash-ref j 'ts "")))

(define (load-history!)
  (with-handlers ([exn:fail? void])
    (define lines
      (call-with-input-file (history-file)
        (lambda (p)
          (for/list ([line (in-lines p)]
                     #:when (non-empty-string? (string-trim line)))
            (with-handlers ([exn:fail? (const #f)])
              (read-json (open-input-string line)))))))
    (for ([j (in-list lines)] #:when (hash? j))
      (define entry
        (with-handlers ([exn:fail? (const #f)]) (history->entry j)))
      (when entry
        (hash-set! consumed-ids (hash-ref entry 'id) #t)
        (hash-set! transfers-table (hash-ref entry 'id) entry)))))

;; ---------------------------------------------------------------- transfer table

(define (make-entry! id direction filename size status message stamp)
  (define entry
    (hasheq 'id id 'direction direction 'filename filename
            'size size 'progress 0 'status status
            'message message 'stamp stamp))
  (call-with-semaphore state-lock
    (lambda () (hash-set! transfers-table id entry)))
  (emit-safe! (lambda () (transfer-changed (entry->record entry))))
  entry)

(define (entry-status id status [message #f] [progress #f] [size #f])
  (define entry
    (call-with-semaphore state-lock
      (lambda () (hash-ref transfers-table id #f))))
  (when entry
    (define next
      (hash-set* entry
                 'status status
                 'message (or message (hash-ref entry 'message ""))
                 'progress (or progress (hash-ref entry 'progress 0))
                 'size (or size (hash-ref entry 'size 0))))
    (call-with-semaphore state-lock
      (lambda () (hash-set! transfers-table id next)))
    (emit-safe! (lambda () (transfer-changed (entry->record next))))))

(define (entry->record entry)
  (Transfer (hash-ref entry 'id)
            (Direction (hash-ref entry 'direction))
            (hash-ref entry 'filename)
            (hash-ref entry 'size)
            (hash-ref entry 'progress)
            (TransferStatus (hash-ref entry 'status))
            (hash-ref entry 'message)
            (hash-ref entry 'stamp)))

(define (snapshot-transfers)
  (call-with-semaphore state-lock
    (lambda () (hash-values transfers-table))))

;; ---------------------------------------------------------------- share operations

;; Creator is owner and owners can always grant themselves more; the
;; (OI)(CI) inheritance flags propagate full control over the whole
;; subtree via the file server — never pass /t, that walks per file.
;;
;; The grant runs IN PROCESS through advapi32's ACL API (v0.1.7): the
;; icacls subprocess popped a visible black terminal on every send, and
;; the conhost --headless wrapper hung the sender thread on Windows 11
;; with terminal delegation. If ffi/unsafe is unavailable in a staged
;; runtime, fall back to a plain icacls call (flashes, but works).
;; Field diagnostics: every share op appends one line here so a hung
;; send on a user machine leaves its exact stopping point on disk.
(define (send-log! what [detail ""])
  (with-handlers ([exn:fail? void])
    (call-with-output-file (build-path (config-dir) "send-debug.log")
      #:exists 'append
      (lambda (p)
        (fprintf p "[~a] ~a~a
"
                 (display-stamp) what
                 (if (string=? detail "") "" (string-append " | " detail)))))))

;; Run THUNK on a worker thread, returning its values; if it exceeds
;; SECONDS the worker is killed and an error is raised. Guards every
;; external call against indefinite hangs (the conhost/icacls lessons).
(define (call-with-hard-timeout seconds thunk)
  (define result-box #f)
  (define error-box #f)
  (define done (make-semaphore))
  (define worker
    (thread (lambda ()
              (with-handlers ([exn:fail?
                               (lambda (e) (set! error-box e) (semaphore-post done))])
                (set! result-box (call-with-values thunk list))
                (semaphore-post done)))))
  (unless (sync/timeout seconds done)
    (kill-thread worker)
    (error 'timeout "operation exceeded ~a seconds" seconds))
  (when error-box (raise error-box))
  (apply values result-box))

(define (icacls-grant! dir)
  (define user (grant-user))
  (and user
       (let ([code (call-with-hard-timeout 5
                     (lambda ()
                       (apply system*/exit-code
                              (build-path (find-system-path 'sys-dir) "icacls.exe")
                              dir "/grant"
                              (string-append user ":(OI)(CI)F"))))])
         (eq? 0 code))))

;; The icacls child is hidden by running it through wscript (a GUI
;; subsystem host: spawning it from our windowed process allocates no
;; console) with a generated .js that shells out with window style 0.
;; Racket CS FFI was tried first but its calls execute on the original
;; thread, which the host's message loop occupies — background-thread
;; FFI hangs forever (v0.1.7). Spawned subprocesses do not have that
;; constraint, and every external call keeps its hard timeout.
(define (wscript-js-path)
  (build-path (config-dir) "shuttle-hide-run.js"))

(define (ensure-wscript-js!)
  (define p (wscript-js-path))
  (unless (file-exists? p)
    (with-output-to-file p #:exists 'truncate
      (lambda ()
        (displayln
         (string-append
          "var sh = new ActiveXObject(\"WScript.Shell\");\n"
          "var c = WScript.Arguments;\n"
          "var line = \"\";\n"
          "for (var i = 1; i < c.length; i++) {\n"
          "  line += (line == \"\" ? \"\" : \" \") + \"\\\"\" + c(i) + \"\\\"\";\n"
          "}\n"
          "WScript.Quit(sh.Run(line, 0, true));\n"))))))

(define (hidden-icacls-grant! dir)
  (define user (grant-user))
  (and user
       (let* ([js (wscript-js-path)]
              [code
               (call-with-hard-timeout 10
                 (lambda ()
                   (system*/exit-code
                    (build-path (find-system-path 'sys-dir) "wscript.exe")
                    "//nologo" js
                    "x" ; slot 0 reserved; arguments start at 1
                    (path->string (build-path (find-system-path 'sys-dir) "icacls.exe"))
                    (path->string dir)
                    "/grant"
                    (string-append user ":(OI)(CI)F"))))])
         (eq? 0 code))))

(define (self-grant-full-control! dir)
  (send-log! "grant begin")
  (ensure-wscript-js!)
  (define r (hidden-icacls-grant! dir))
  (send-log! "grant result" (if r "ok" (format "code=~a" r)))
  r)

(define (delete-tree-with-retries! path [attempts 3])
  (let loop ([n attempts])
    (cond
      [(not (directory-exists? path)) #t]
      [(delete-directory/files path #:must-exist? #f) #t]
      [(<= n 1) #f]
      [else (sleep 0.5) (loop (sub1 n))])))

;; Copy in 4 MiB chunks, emitting progress at most every 150 ms.
(define (copy-with-progress! src dest id)
  (define chunk (make-bytes (* 4 1024 1024)))
  (define total (file-size src))
  (define copied 0)
  (define last-emit (current-inexact-milliseconds))
  (call-with-output-file dest #:exists 'truncate
    (lambda (out)
      (call-with-input-file src
        (lambda (in)
          (let loop ()
            (define n (read-bytes-avail! chunk in))
            (unless (eof-object? n)
              (write-bytes chunk out 0 n)
              (set! copied (+ copied n))
              (define now (current-inexact-milliseconds))
              (when (>= (- now last-emit) 150)
                (set! last-emit now)
                (entry-status id 'active #f copied total))
              (loop)))))))
  (entry-status id 'active #f total total)
  total)

;; ---------------------------------------------------------------- mailbox format

;; Subfolder: <yyyyMMdd-HHmmss>-<id8>/
;;   manifest.json  (written first, so crashed sends stay attributable)
;;   payload.part -> payload  (renamed after the full copy)
;;   shuttle.done   (written last; receivers only act on folders with it)

(define (mailbox-folder-name stamp id)
  (string-append stamp "-" (substring id 0 (min 8 (string-length id)))))

(define (write-manifest! dir sender filename size id stamp)
  (call-with-output-file (build-path dir "manifest.json") #:exists 'truncate
    (lambda (p)
      (write-json (hasheq 'shuttle 1
                          'sender sender
                          'file filename
                          'size size
                          'id id
                          'stamp stamp)
                  p))))

(define (read-manifest dir)
  (with-handlers ([exn:fail? (const #f)])
    (define j (call-with-input-file (build-path dir "manifest.json") read-json))
    (and (hash? j) (equal? (hash-ref j 'shuttle #f) 1) j)))

(define (write-done-marker! dir)
  (call-with-output-file (build-path dir "shuttle.done") #:exists 'truncate void))

(define (manifest-has-done? dir)
  (file-exists? (build-path dir "shuttle.done")))

;; ---------------------------------------------------------------- sending

(define (do-send! id local-path)
  (define relay (string->path (hash-ref (config) 'relay-dir)))
  (define src (string->path local-path))
  (define stamp (display-stamp))
  (define entry (call-with-semaphore state-lock
                  (lambda () (hash-ref transfers-table id #f))))
  (define filename (hash-ref entry 'filename))
  (define size (hash-ref entry 'size))

  (define (fail! msg)
    (entry-status id 'failed msg)
    (append-history! (hasheq 'id id 'direction "send" 'file filename
                             'size size 'ts stamp 'ok #f 'msg msg)))

  (send-log! "send begin" (format "id=~a file=~a bytes=~a" id filename size))
  (with-handlers ([exn:fail?
                   (lambda (e)
                     (send-log! "send FAILED" (exn-message e))
                     (fail! (format "发送失败：~a" (exn-message e))))])
    (unless (file-exists? src)
      (error 'shuttle "源文件不存在：~a" local-path))
    (send-log! "relay mkdir")
    (make-directory* relay)
    (define folder (build-path relay (mailbox-folder-name (folder-stamp) id)))
    (send-log! "folder mkdir")
    (make-directory* folder)
    (send-log! "self-grant")
    (self-grant-full-control! folder)
    (send-log! "self-grant done")
    ;; Manifest first: a crashed send still shows sender attribution for sweep.
    (write-manifest! folder (this-machine) filename size id stamp)
    (entry-status id 'active "复制到信箱中…")
    (send-log! "copy begin")
    (copy-with-progress! src (build-path folder "payload.part") id)
    (rename-file-or-directory (build-path folder "payload.part")
                              (build-path folder "payload"))
    (write-done-marker! folder)
    (send-log! "send complete")
    (entry-status id 'sent "已放入信箱，待另一台电脑接收")
    (append-history! (hasheq 'id id 'direction "send" 'file filename
                             'size size 'ts stamp 'ok #t 'msg "已发送"))))

(define (sender-loop)
  (let loop ()
    (define item (async-channel-get send-queue))
    ;; Queue items are (cons id path); a malformed legacy item fails
    ;; this one task without killing the sender thread.
    (with-handlers ([exn:fail?
                     (lambda (e)
                       (define id (if (pair? item) (car item) item))
                       (entry-status id 'failed
                                     (format "发送线程错误：~a" (exn-message e))))])
      (define id (car item))
      (define path (cdr item))
      (unless (string? path)
        (error 'shuttle "队列缺少文件路径"))
      (do-send! id path))
    (loop)))

;; ---------------------------------------------------------------- receiving

;; Emit only on change so the UI is not spammed each poll.
(define (update-relay-health! ok message)
  (define last-ok (config-relay-ok))
  (unless (eq? ok last-ok)
    (config-relay-ok-param ok)
    (emit-safe! (lambda () (relay-status (RelayHealth ok message))))))

;; #t paths keep everything absolute; relative paths would break on SMB.
(define (relay-folders relay)
  (with-handlers ([exn:fail? (const '())])
    (sort (filter directory-exists? (directory-list relay #:build? #t))
          string<? #:key path->string)))

(define (receive-cycle!)
  (define relay (string->path (hash-ref (config) 'relay-dir)))
  (define downloads (string->path (hash-ref (config) 'download-dir)))

  (cond
    [(not (directory-exists? relay))
     (with-handlers ([exn:fail? void]) (make-directory* relay))
     (update-relay-health!
      (directory-exists? relay)
      "中转目录不可达，请检查共享盘映射（Z:）")]
    [else
     (update-relay-health! #t "信箱在线")
     (with-handlers ([exn:fail? void]) (make-directory* downloads))
     (for ([folder (in-list (relay-folders relay))])
       (try-receive! folder downloads))]))

(define (try-receive! folder downloads)
  (define manifest (read-manifest folder))
  (when (and manifest (manifest-has-done? folder))
    (define id (hash-ref manifest 'id "unknown"))
    (unless (hash-ref consumed-ids id #f)
      (unless (string=? (hash-ref manifest 'sender "") (this-machine))
        ;; Our own sends wait for the other PC; everything else is incoming.
        (receive-folder! folder manifest id downloads)))))

(define (receive-folder! folder manifest id downloads)
  (define sender (hash-ref manifest 'sender "未知电脑"))
  (define raw-name (hash-ref manifest 'file ""))
  (define size (hash-ref manifest 'size 0))
  (define stamp (display-stamp))

  (let/ec bail
    (define (finish! status message)
      (entry-status id status message)
      (mark-consumed! id)
      (append-history! (hasheq 'id id 'direction "receive" 'file raw-name
                               'size size 'ts stamp
                               'ok (eq? status 'received)
                               'msg message))
      (bail (void)))

    (define safe-name (sanitize-filename raw-name))
    (unless (string? safe-name)
      (finish! 'failed "清单中的文件名无效，已跳过"))
    (unless (exact-nonnegative-integer? size)
      (finish! 'failed "清单中的大小无效，已跳过"))

    (make-entry! id 'receive safe-name size 'active "接收中…" stamp)
    (with-handlers ([exn:fail?
                     (lambda (e)
                       (finish! 'failed (format "接收失败：~a" (exn-message e))))])
      (define payload (build-path folder "payload"))
      (unless (file-exists? payload)
        (finish! 'failed "信箱中缺少文件内容，已跳过"))
      (define dest (unique-dest downloads safe-name))
      (define copied (copy-with-progress! payload dest id))
      (unless (= copied size)
        (with-handlers ([exn:fail? void]) (delete-file dest))
        (finish! 'failed "大小校验不一致，已丢弃本地副本"))
      ;; Permanent delete on the share — only after the local copy is
      ;; fully written and verified.
      (define removed (delete-tree-with-retries! folder))
      (finish! 'received
               (format "来自 ~a，已保存~a"
                       sender
                       (if removed "" "；信箱残留请手动清理"))))))

;; Startup / hourly sweep: only folders whose manifest names THIS machine
;; as sender are ever removed. The mailbox is a relay, not storage — the
;; original file stays wherever the user sent it from.
(define (sweep-own-folders! relay)
  (define now-seconds (current-seconds))
  (for ([folder (in-list (relay-folders relay))])
    (define manifest (read-manifest folder))
    (when (and manifest
               (string=? (hash-ref manifest 'sender "") (this-machine)))
      (define name (path->string folder))
      (define m (regexp-match #rx"([0-9]{8})-([0-9]{6})-[0-9a-f]+$" name))
      (when m
        (define age-hours
          (folder-age-hours (second m) (third m) now-seconds))
        ;; A folder without .done is a crashed send — reclaim after a day.
        ;; A completed send is kept a week for the other PC to pick up.
        (define threshold (if (manifest-has-done? folder) (* 24 7) 24))
        (when (>= age-hours threshold)
          (delete-tree-with-retries! folder))))))

(define (folder-age-hours ymd hms now-seconds)
  (define then
    (find-seconds (string->number (substring hms 4 6))
                  (string->number (substring hms 2 4))
                  (string->number (substring hms 0 2))
                  (string->number (substring ymd 6 8))
                  (string->number (substring ymd 4 6))
                  (string->number (substring ymd 0 4))
                  #f #f))
  (/ (- now-seconds then) 3600.0))

(define (receiver-loop)
  (let loop ()
    (with-handlers ([exn:fail? void])
      (receive-cycle!))
    (sleep 1.5)
    (loop)))

;; ---------------------------------------------------------------- RPC layer

(define-rpc (ping : String)
  (format "~a / shuttle ready" (this-machine)))

;; The send-file body, factored out so tests can exercise the exact
;; RPC path — including the declared Transfer RESULT type. Returning
;; the raw entry hasheq here made the wire validation reject every
;; send ("expected Transfer received Unsupported") after the transfer
;; had already been queued.
(define (begin-send! path)
  (unless (file-exists? path)
    (error 'shuttle "文件不存在：~a" path))
  (when (directory-exists? path)
    (error 'shuttle "暂不支持发送文件夹：~a" path))
  (define base (file-name-from-path (string->path path)))
  (define safe-name (or (sanitize-filename path)
                        (and base (path->string base))))
  (unless safe-name
    (error 'shuttle "文件名无效：~a" path))
  (define id (new-transfer-id))
  (define stamp (display-stamp))
  (define size (file-size path))
  (define entry
    (make-entry! id 'send safe-name size 'queued "排队中…" stamp))
  (async-channel-put send-queue (cons id path))
  (entry->record entry))

(define-rpc (send-file [path : String] : Transfer)
  (begin-send! path))

;; The list-transfers body, factored out for tests: the sort key must
;; default because entries loaded from the ledger predate normalization.
(define (list-transfers-records)
  (for/list ([entry (in-list
                     (sort (snapshot-transfers) string>?
                           #:key (lambda (e) (hash-ref e 'stamp ""))))])
    (entry->record entry)))

(define-rpc (list-transfers : (List Transfer))
  (list-transfers-records))

(define-rpc (clear-history : Void)
  (call-with-semaphore state-lock
    (lambda () (hash-clear! transfers-table)))
  (with-handlers ([exn:fail? void])
    (delete-file (history-file)))
  (void))

(define-rpc (get-settings : Settings)
  (settings-record))

(define-rpc (set-settings [relay-dir : String] [download-dir : String] : Settings)
  (define relay (string->path (string-trim relay-dir)))
  (define downloads (string->path (string-trim download-dir)))
  (with-handlers ([exn:fail?
                   (lambda (e)
                     (error 'shuttle "无法创建中转目录：~a" (exn-message e)))])
    (make-directory* relay))
  (with-handlers ([exn:fail?
                   (lambda (e)
                     (error 'shuttle "无法创建下载目录：~a" (exn-message e)))])
    (make-directory* downloads))
  (config (hasheq 'relay-dir (path->display relay)
                  'download-dir (path->display downloads)))
  (write-config! (config))
  (config-relay-ok-param 'unknown) ; force a fresh health event on next poll
  (settings-record))

(define (settings-record)
  (define cfg (config))
  (Settings (hash-ref cfg 'relay-dir)
            (hash-ref cfg 'download-dir)
            (this-machine)))

;; ---------------------------------------------------------------- lifecycle

(define (init! #:config-dir [dir #f]
               #:relay-dir [relay #f]
               #:download-dir [downloads #f]
               #:force? [force? #f])
  ;; Self-tests re-point everything per case with #:force? #t; a real
  ;; process initializes exactly once.
  (when (or force? (not ready?))
    (set! ready? #t)
    (hash-clear! transfers-table)
    (config-dir-param
     (or dir
         (let ([appdata (getenv "APPDATA")])
           (if appdata
               (build-path appdata "Shuttle")
               (build-path (find-system-path 'home-dir) "Shuttle")))))
    (load-or-create-config!)
    (when relay
      (config (hash-set* (config) 'relay-dir (path->display relay))))
    (when downloads
      (config (hash-set* (config) 'download-dir (path->display downloads))))
    (load-consumed!)
    (load-history!)))

(define (start-threads!)
  (thread sender-loop)
  (thread receiver-loop)
  (thread
   (lambda ()
     (let loop ()
       (with-handlers ([exn:fail? void])
         (sweep-own-folders! (string->path (hash-ref (config) 'relay-dir))))
       (sleep (* 60 60))))))

(define (start in-fd out-fd)
  (init!)
  (start-threads!)
  (serve-fds in-fd out-fd))

;; ---------------------------------------------------------------- tests

(module+ test
  (require rackunit
           racket/port
           racket/string)
  (provide test-main)

  ;; --- pure helpers -----------------------------------------------
  (test-case "sanitize-filename"
    (check-equal? (sanitize-filename "报告.zip") "报告.zip")
    (check-equal? (sanitize-filename "C:\\evil\\..\\x.txt") "x.txt")
    (check-equal? (sanitize-filename "..") #f)
    (check-equal? (sanitize-filename ".") #f)
    (check-equal? (sanitize-filename "") #f)
    (check-equal? (sanitize-filename "bad<>|name.bin") "bad___name.bin"))

  (test-case "unique-dest"
    (define tmp (make-temporary-file "shut~a" 'directory))
    (define first-path (unique-dest tmp "a.bin"))
    (check-false (file-exists? first-path))
    (call-with-output-file first-path void #:exists 'truncate)
    (check-equal? (path->string (file-name-from-path (unique-dest tmp "a.bin")))
                  "a (2).bin")
    (delete-directory/files tmp))

  (test-case "manifest roundtrip"
    (define tmp (make-temporary-file "shut~a" 'directory))
    (write-manifest! tmp "PEER-PC" "hello.zip" 123 "abcd1234" "20261027-180000")
    (define m (read-manifest tmp))
    (check-equal? (hash-ref m 'sender) "PEER-PC")
    (check-equal? (hash-ref m 'file) "hello.zip")
    (check-equal? (hash-ref m 'size) 123)
    (delete-directory/files tmp))

  ;; --- full mailbox chain against the real share ------------------
  ;; Uses a dedicated self-test mailbox so real mail is never touched.
  ;; The send runs under a fake machine identity, then receive-cycle!
  ;; (as this machine) must pick it up, verify, and clean the share.
  (define (run-mailbox-selftest!)
    (define share-root
      (string->path "Z:\\19 汽车电子事业部\\研发部\\04_Software\\LiDAR"))
    (define relay (build-path share-root ".shuttle-selftest"))
    (define downloads (make-temporary-file "shuttle-dl~a" 'directory))
    (init! #:force? #t
           #:config-dir (make-temporary-file "shuttle-cfg~a" 'directory)
           #:relay-dir relay #:download-dir downloads)

    ;; 1. send a small file under a fake peer identity
    (define src (make-temporary-file "shuttle-src~a"))
    (define payload #"shuttle selftest payload \xc3\xa9\xc3\xa8")
    (call-with-output-file src #:exists 'truncate
      (lambda (p) (write-bytes payload p)))
    (define test-id (new-transfer-id))
    (make-entry! test-id 'send "selftest 报告.bin"
                 (file-size src) 'queued "测试" (display-stamp))
    (parameterize ([this-machine "SELFTEST-PEER"])
      (do-send! test-id (path->string src)))

    ;; 2. the mailbox must now hold one folder with payload + markers
    (define ours
      (for/list ([f (in-list (relay-folders relay))]
                 #:when (equal? (hash-ref (read-manifest f) 'id #f) test-id))
        f))
    (check-equal? (length ours) 1 "exactly one mailbox folder for the test id")
    (define folder (first ours))
    (check-true (file-exists? (build-path folder "payload")))
    (check-true (file-exists? (build-path folder "shuttle.done")))
    (check-equal? (file-size (build-path folder "payload")) (file-size src))

    ;; 3. icacls self-heal: our grant must show up as inherited full control
    (define acl-out
      (with-output-to-string
        (lambda ()
          (system* (build-path (find-system-path 'sys-dir) "icacls.exe")
                   (build-path folder "payload")))))
    (check-true (string-contains? acl-out "(I)(F)")
                (string-append "payload must inherit (I)(F), got: " acl-out))

    ;; 4. receiving as this machine: download, verify, clean up the share
    (receive-cycle!)
    (define dests (map path->string (directory-list downloads)))
    (check-equal? (length dests) 1 (format "downloaded: ~a" dests))
    (check-equal? (file->bytes (build-path downloads (first dests))) payload)
    (check-false (directory-exists? folder) "remote folder cleaned up")
    (check-true (file-exists? (build-path (config-dir) "history.jsonl"))
                "history persisted")

    (delete-directory/files downloads)
    (delete-directory/files relay)
    (delete-file src))

  ;; The send path as the RPC actually drives it: send-file puts
  ;; (cons id path) on the queue and a real sender-loop thread drains
  ;; it. Guards the arity regression where the loop passed only the id
  ;; to do-send! and every send failed at runtime.
  ;; The RPC result type regression: begin-send! (the send-file body)
  ;; must return a Transfer RECORD. Returning the raw entry hasheq made
  ;; the wire layer reject every send at runtime while unit tests that
  ;; bypassed the RPC body stayed green.
  (test-case "begin-send! returns a Transfer record"
    (define tmp (make-temporary-file "shuttle-rpc~a" 'directory))
    (init! #:force? #t
           #:config-dir (make-temporary-file "shuttle-cfg~a" 'directory)
           #:relay-dir (build-path tmp "relay")
           #:download-dir (build-path tmp "dl"))
    (define src (make-temporary-file "shuttle-src~a"))
    (call-with-output-file src #:exists 'truncate
      (lambda (p) (display "x" p)))
    (define result (begin-send! (path->string src)))
    ;; record-ref raises on a non-record; a raw hasheq must not pass.
    (check-true (string? (record-ref result 'id))
                "send-file result must be a Transfer record")
    (check-equal? (record-ref result 'filename)
                  (path->string (file-name-from-path src)))
    (check-equal? (record-ref result 'status) (TransferStatus 'queued))
    (delete-file src)
    (delete-directory/files tmp))

  ;; The startup crash regression (v0.1.5): ledger rows carry the
  ;; ledger schema (file/ts/msg) and never 'stamp. load-history! must
  ;; normalize them into entries so list-transfers' sort and record
  ;; projection survive a restart after any real transfer; rows
  ;; without an id are skipped.
  (test-case "ledger history loads into normalized entries"
    (define cfg (make-temporary-file "shuttle-ledger~a" 'directory))
    (define tmp (make-temporary-file "shuttle-ledger-relay~a" 'directory))
    (call-with-output-file (build-path cfg "history.jsonl") #:exists 'truncate
      (lambda (p)
        (displayln "{\"id\":\"r1\",\"direction\":\"send\",\"file\":\"a.zip\",\"size\":5,\"ts\":\"2026-10-29 18:00:00\",\"ok\":true,\"msg\":\"已发送\"}" p)
        (displayln "{\"id\":\"r2\",\"direction\":\"receive\",\"file\":\"b.bin\",\"size\":9,\"ts\":\"2026-10-29 18:01:00\",\"ok\":false,\"msg\":\"校验不一致\"}" p)
        (displayln "{\"direction\":\"receive\",\"file\":\"orphan.bin\"}" p)))
    (init! #:force? #t
           #:config-dir cfg
           #:relay-dir (build-path tmp "relay")
           #:download-dir (build-path tmp "dl"))
    (define records (list-transfers-records))
    (check-equal? (length records) 2 "orphan row skipped, two normalized")
    (define (record-with-id id)
      (for/first ([r (in-list records)]
                  #:when (string=? (record-ref r 'id) id))
        r))
    (define r1 (record-with-id "r1"))
    (check-true (and r1 #t) "normalized row r1 must be present")
    (check-equal? (record-ref r1 'filename) "a.zip")
    (check-equal? (record-ref r1 'stamp) "2026-10-29 18:00:00")
    (check-equal? (record-ref r1 'status) (TransferStatus 'sent))
    (define r2 (record-with-id "r2"))
    (check-true (and r2 #t) "normalized row r2 must be present")
    (check-equal? (record-ref r2 'status) (TransferStatus 'failed))
    (check-equal? (record-ref r2 'direction) (Direction 'receive))
    (delete-directory/files cfg)
    (delete-directory/files tmp))

  (define (run-send-loop-selftest!)
    (define share-root
      (string->path "Z:\\19 汽车电子事业部\\研发部\\04_Software\\LiDAR"))
    (define relay (build-path share-root ".shuttle-selftest"))
    (define downloads (make-temporary-file "shuttle-dl~a" 'directory))
    (init! #:force? #t
           #:config-dir (make-temporary-file "shuttle-cfg~a" 'directory)
           #:relay-dir relay #:download-dir downloads)

    (define sender-thread (thread sender-loop))
    (define entry (send-file-for-test! (path->string
                                        (make-temporary-file "shuttle-loop~a"))))
    (define deadline (+ (current-seconds) 30))
    (let wait ()
      (define current
        (for/first ([e (in-list (snapshot-transfers))]
                    #:when (string=? (hash-ref e 'id) (hash-ref entry 'id)))
          e))
      (define status (and current (hash-ref current 'status)))
      (cond
        [(member status '(sent failed))
         (check-equal? status 'sent
                       (format "send via loop: ~a"
                               (and current (hash-ref current 'message))))]
        [(> (current-seconds) deadline)
         (check-false #t "sender loop never settled the transfer")]
        [else (sleep 0.3) (wait)]))

    (kill-thread sender-thread)
    (delete-directory/files downloads)
    (delete-directory/files relay))

  ;; send-file minus the rivet RPC wrapper (same body): validates the
  ;; file, files the entry, enqueues (cons id path).
  (define (send-file-for-test! path)
    (define base (file-name-from-path (string->path path)))
    (define safe-name (or (sanitize-filename path)
                          (and base (path->string base))))
    (define id (new-transfer-id))
    (define size (file-size (string->path path)))
    (make-entry! id 'send safe-name size 'queued "排队中…" (display-stamp))
    (async-channel-put send-queue (cons id path))
    (hasheq 'id id))

  (define (test-main)
    (run-mailbox-selftest!)
    (run-send-loop-selftest!)))