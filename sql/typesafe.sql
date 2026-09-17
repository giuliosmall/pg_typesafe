--
-- typesafe
--
CREATE EXTENSION typesafe;

-- Classify a support ticket (Choice)
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "label": {
      "type": "choice",
      "choice": "technical",
      "confidence": 0.82,
      "probabilities": {
        "billing": 0.08,
        "technical": 0.85,
        "sales": 0.07
      }
    }
  },
  "usage": {"input_tokens": 312, "output_tokens": 48}
}$$;

SELECT * FROM typesafe_classify(
	'Help! My payouts have been failing for 3 days.',
	'Which team should handle this?',
	'{"billing": "Payments, invoicing, refunds", "technical": "Bugs, outages, integrations", "sales": "Pricing, upgrades, new accounts"}'::jsonb);

SELECT typesafe_label(
	'Help! My payouts have been failing for 3 days.',
	'Which team should handle this?',
	'{"billing": "Payments, invoicing, refunds", "technical": "Bugs, outages, integrations", "sales": "Pricing, upgrades, new accounts"}'::jsonb);

SELECT typesafe_last_request();

-- Detect urgency (Noul)
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "flag": {
      "type": "noul",
      "noul": 0.92
    }
  },
  "usage": {"input_tokens": 312, "output_tokens": 48}
}$$;

SELECT * FROM typesafe_detect(
	'Help! My payouts have been failing for 3 days.',
	'Does this convey urgency?',
	'Explicitly time-sensitive',
	'No urgency expressed');

SELECT typesafe_noul(
	'Help! My payouts have been failing for 3 days.',
	'Does this convey urgency?',
	'Explicitly time-sensitive',
	'No urgency expressed');

-- Score frustration (Score)
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "rating": {
      "type": "score",
      "score": 1.6,
      "confidence": 0.78,
      "legend": {"0": "Calm", "1": "Frustrated", "2": "Very angry"},
      "probabilities": {"0": 0.05, "1": 0.3, "2": 0.65}
    }
  },
  "usage": {"input_tokens": 312, "output_tokens": 48}
}$$;

SELECT * FROM typesafe_score(
	'Help! My payouts have been failing for 3 days.',
	'How frustrated is the customer?',
	ARRAY['Calm', 'Frustrated', 'Very angry']);

-- Ask two questions (choice + noul)
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "department": {
      "type": "choice",
      "choice": "technical",
      "confidence": 0.82,
      "probabilities": {
        "billing": 0.08,
        "technical": 0.85,
        "sales": 0.07
      }
    },
    "is_urgent": {
      "type": "noul",
      "noul": 0.92
    }
  },
  "usage": {"input_tokens": 312, "output_tokens": 48}
}$$;

SELECT jsonb_object_keys(typesafe_ask(
	'{"ticket": "Help! My payouts have been failing for 3 days."}'::jsonb,
	'{"department": {"type": "choice", "instructions": "Which team should handle this?", "criteria": {"billing": "Payments, invoicing, refunds", "technical": "Bugs, outages, integrations", "sales": "Pricing, upgrades, new accounts"}}, "is_urgent": {"type": "noul", "instructions": "Does this convey urgency?"}}'::jsonb)->'answers') AS answer_key
ORDER BY 1;

-- Batch detect (one TypeSafe request, many Nouls)
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "s0": {"type": "noul", "noul": 0.91},
    "s1": {"type": "noul", "noul": 0.02}
  },
  "usage": {"input_tokens": 400, "output_tokens": 24}
}$$;

SELECT ordinality, state, noul
FROM typesafe_detect_many(
	ARRAY[
		'The Department of Sanitation couldn''t find the condition.',
		'The City has removed the graffiti from this property.'
	],
	'Does this resolution say the condition could not be found?');

SELECT typesafe_last_request() LIKE '%"s0"%' AND typesafe_last_request() LIKE '%"s1"%' AS packed_two_questions;

SELECT ordinality, noul IS NULL AS noul_is_null
FROM typesafe_detect_many(
	ARRAY['The Department of Sanitation couldn''t find the condition.', NULL, 'The City has removed the graffiti from this property.'],
	'Does this resolution say the condition could not be found?')
ORDER BY ordinality;

-- Batch classify
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "s0": {
      "type": "choice",
      "choice": "technical",
      "confidence": 0.82,
      "probabilities": {"billing": 0.08, "technical": 0.85, "sales": 0.07}
    },
    "s1": {
      "type": "choice",
      "choice": "billing",
      "confidence": 0.70,
      "probabilities": {"billing": 0.70, "technical": 0.20, "sales": 0.10}
    }
  },
  "usage": {"input_tokens": 400, "output_tokens": 24}
}$$;

SELECT ordinality, choice
FROM typesafe_classify_many(
	ARRAY[
		'Help! My payouts have been failing for 3 days.',
		'I was charged twice, where is my refund?'
	],
	'Which team should handle this?',
	'{"billing": "Payments, invoicing, refunds", "technical": "Bugs, outages, integrations", "sales": "Pricing, upgrades, new accounts"}'::jsonb)
ORDER BY ordinality;

-- NULL state
SELECT typesafe_classify(
	NULL,
	'Which team should handle this?',
	'{"billing": "Payments, invoicing, refunds", "technical": "Bugs, outages, integrations", "sales": "Pricing, upgrades, new accounts"}'::jsonb);

-- options must be a JSON object, not an array
SELECT typesafe_classify(
	'Help! My payouts have been failing for 3 days.',
	'Which team should handle this?',
	'["billing", "technical", "sales"]'::jsonb);

-- levels must have at least two elements
SELECT typesafe_score(
	'Help! My payouts have been failing for 3 days.',
	'How frustrated is the customer?',
	ARRAY['Calm']);

-- empty mock_response and empty api_key
SET typesafe.mock_response = '';
SET typesafe.api_key = '';
SELECT typesafe_classify(
	'Help! My payouts have been failing for 3 days.',
	'Which team should handle this?',
	'{"billing": "Payments, invoicing, refunds", "technical": "Bugs, outages, integrations", "sales": "Pricing, upgrades, new accounts"}'::jsonb);

-- EXECUTE is revoked from PUBLIC
DROP ROLE IF EXISTS typesafe_nobody;
CREATE ROLE typesafe_nobody NOLOGIN;
SET SESSION AUTHORIZATION typesafe_nobody;
SELECT typesafe_noul('x', 'y');
RESET SESSION AUTHORIZATION;
DROP ROLE typesafe_nobody;
