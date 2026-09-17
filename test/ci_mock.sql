CREATE EXTENSION IF NOT EXISTS typesafe;

SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "flag": {"type": "noul", "noul": 0.92}
  },
  "usage": {"input_tokens": 1, "output_tokens": 1}
}$$;

SELECT typesafe_noul('ticket', 'Does this convey urgency?') AS noul;

SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "s0": {"type": "noul", "noul": 0.91},
    "s1": {"type": "noul", "noul": 0.02}
  },
  "usage": {"input_tokens": 1, "output_tokens": 1}
}$$;

SELECT ordinality, noul
FROM typesafe_detect_many(
	ARRAY['could not find the condition', 'graffiti removed'],
	'Could the condition not be found?'
)
ORDER BY 1;
