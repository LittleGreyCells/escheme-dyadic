(begin
  (load "./tests/escheme_test_suite.scm")
  (perform-timed-run 100)
  (load "./tests/escheme_test_suite2.scm")
  (perform-timed-run 100)
  )
