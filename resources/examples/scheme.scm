#lang r7rs
;; The same language family in R7RS Scheme.
(define (square x) (* x x))
(define (factorial n)
  (if (<= n 1) 1 (* n (factorial (- n 1)))))
(display (square 9)) (newline)
(display (factorial 10)) (newline)
