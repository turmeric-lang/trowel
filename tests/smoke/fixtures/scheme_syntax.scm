; R7RS lexemes the scanner has to recognize, and Turmeric syntax it must NOT
; read as Turmeric in a Scheme buffer. No `#lang` line on purpose: `.scm`
; selects the Scheme language and reader by extension upstream
; (reader_type_from_extension), so this file is the headerless case.
(define scm-bool #true)
(define scm-bool-short #f)
(define scm-char #\x41)
(define scm-named-char #\space)
(define scm-vec #(1 2 3))
(define scm-bytes #u8(1 2 3))
(define scm-hex #xff)
(define scm-exact #e1.5)
(define scm-ratio 1/2)
(define |a bar symbol| 7)
(define scm-quasi `(a ,scm-hex ,@(list 1 2)))
(define-record-type point (make-point x y) point? (x point-x) (y point-y))
(define (scm-show v) (display v) (newline))
(define scm-colon-id :not-a-keyword-literal)
(define scm-pipe-gt 'no-pipe-operator-here)
#| a block comment, shared with Turmeric |#
#;(a datum comment, also shared)
