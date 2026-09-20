# Copyright (c) 2026, Giulio Piccolo
#
# Hardening coverage: retry-on-429 (with Retry-After), multi-request
# batches, response size cap, endpoint policy, malformed responses,
# timeouts, and GUC privilege.

use strict;
use warnings FATAL => 'all';

use IO::Socket::INET;
use POSIX qw(_exit);
use Time::HiRes ();
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

delete $ENV{TYPESAFE_API_KEY};

my $node = PostgreSQL::Test::Cluster->new('typesafe_hard');
$node->init;
$node->start;

my ($ret, $stdout, $stderr) =
  $node->psql('postgres', 'CREATE EXTENSION typesafe;');
if ($ret != 0)
{
	plan skip_all => 'typesafe extension is not available';
}

my $server = IO::Socket::INET->new(
	LocalAddr => '127.0.0.1',
	LocalPort => 0,
	Listen => 16,
	Proto => 'tcp',
	ReuseAddr => 1);
BAIL_OUT("could not listen on 127.0.0.1: $!") unless $server;

my $port = $server->sockport();
my $child_pid = fork();
BAIL_OUT("fork failed: $!") unless defined $child_pid;

if ($child_pid == 0)
{
	$SIG{TERM} = $SIG{INT} = sub { _exit(0); };
	$SIG{PIPE} = 'IGNORE';
	eval { mock_http($server); };
	_exit(0);
}
close $server;

END
{
	if (defined $child_pid && $child_pid > 0)
	{
		kill 'TERM', $child_pid;
		waitpid $child_pid, 0;
	}
}

my $base = "http://127.0.0.1:$port";

sub run_sql
{
	my ($path, $extra, $sql) = @_;
	$extra = '' unless defined $extra;
	return $node->psql('postgres',
		    "SET typesafe.api_key = 'test-key';\n"
		  . "SET typesafe.endpoint = '$base$path';\n"
		  . "SET typesafe.mock_response = '';\n"
		  . "SET typesafe.timeout_ms = 5000;\n"
		  . $extra
		  . $sql);
}

# --- 429 twice (Retry-After: 1) then success ---------------------------
my $t0 = Time::HiRes::time();
($ret, $stdout, $stderr) =
  run_sql('/flaky', '', "SELECT typesafe_noul('x', 'urgent?');");
my $elapsed = Time::HiRes::time() - $t0;
is($ret, 0, '429/Retry-After: request eventually succeeds');
like($stdout, qr/0\.42/, '429 retry returns the real answer');

# Two 429s with Retry-After: 1 mean >=2s of waiting; plain exponential
# backoff alone would be 0.2 + 0.4 = 0.6s.  1.5s cleanly separates the two.
cmp_ok($elapsed, '>=', 1.5, '429 retry honored Retry-After');

# --- retry exhaustion: always 429 --------------------------------------
($ret, $stdout, $stderr) =
  run_sql('/always429', '', "SELECT typesafe_noul('x', 'urgent?');");
isnt($ret, 0, 'permanent 429 fails after retries');
like($stderr, qr/status 429/, 'permanent 429 surfaces the status');

# --- batch across the multi loop: 5 texts, batch_size 2 = 3 requests ---
($ret, $stdout, $stderr) = run_sql('/ok',
	"SET typesafe.batch_size = 2;\nSET typesafe.http_concurrency = 4;\n",
	"SELECT count(*), count(noul), sum(input_tokens)"
	  . " FROM typesafe_detect_many(ARRAY['a','b','c','d','e'], 'q?');");
is($ret, 0, 'batched detect_many over live multi loop succeeds');
like($stdout, qr/^5\|5\|21$/m,
	'5 rows, 5 answers, usage summed once per chunk (3 chunks x 7)');

# --- oversized response is refused -------------------------------------
($ret, $stdout, $stderr) =
  run_sql('/huge', '', "SELECT typesafe_noul('x', 'urgent?');");
isnt($ret, 0, 'oversized response fails');
like($stderr, qr/exceeds/, 'oversized response reports the size cap');

# --- garbage and wrong-shape responses ---------------------------------
($ret, $stdout, $stderr) =
  run_sql('/garbage', '', "SELECT typesafe_noul('x', 'urgent?');");
isnt($ret, 0, 'non-JSON response fails');

($ret, $stdout, $stderr) =
  run_sql('/wrongshape', '', "SELECT typesafe_noul('x', 'urgent?');");
isnt($ret, 0, 'JSON without answers fails');
like($stderr, qr/invalid typesafe response/, 'shape error is reported');

# --- server that stalls: timeout_ms is honored -------------------------
($ret, $stdout, $stderr) = $node->psql('postgres',
	    "SET typesafe.api_key = 'test-key';\n"
	  . "SET typesafe.endpoint = '$base/stall';\n"
	  . "SET typesafe.mock_response = '';\n"
	  . "SET typesafe.timeout_ms = 1000;\n"
	  . "SELECT typesafe_noul('x', 'urgent?');");
isnt($ret, 0, 'stalled server fails');
like($stderr, qr/Timeout|timed out/i, 'stall surfaces as timeout');

# --- endpoint policy ----------------------------------------------------
($ret, $stdout, $stderr) = $node->psql('postgres',
	    "SET typesafe.api_key = 'test-key';\n"
	  . "SET typesafe.endpoint = 'http://93.184.216.34/';\n"
	  . "SET typesafe.mock_response = '';\n"
	  . "SELECT typesafe_noul('x', 'urgent?');");
isnt($ret, 0, 'non-local http endpoint fails');
like($stderr, qr/must use https/, 'endpoint policy error is clear');

$node->stop;
done_testing();

# ---------------------------------------------------------------------
sub mock_http
{
	my ($srv) = @_;
	my $flaky_count = 0;

	while (1)
	{
		my $c = $srv->accept();
		next unless $c;
		$c->autoflush(1);
		eval {
			my ($headers, $body) = read_http($c);
			my ($path) = $headers =~ m{^POST\s+(\S+)}i;
			$path = '/' unless defined $path;

			if ($headers !~ /Authorization:\s*Bearer\s+test-key\b/i)
			{
				http_reply($c, '401 Unauthorized', '{"error":"bad key"}');
			}
			elsif ($path eq '/flaky' && ++$flaky_count <= 2)
			{
				http_reply($c, '429 Too Many Requests',
					'{"error":"rate limited"}',
					"Retry-After: 1\r\n");
			}
			elsif ($path eq '/always429')
			{
				http_reply($c, '429 Too Many Requests',
					'{"error":"rate limited"}');
			}
			elsif ($path eq '/huge')
			{
				http_reply($c, '200 OK',
					'{"model":"m","pad":"' . ('x' x (9 * 1024 * 1024)) . '"}');
			}
			elsif ($path eq '/garbage')
			{
				http_reply($c, '200 OK', 'this is not json {{{');
			}
			elsif ($path eq '/wrongshape')
			{
				http_reply($c, '200 OK', '{"model":"jev-latest"}');
			}
			elsif ($path eq '/stall')
			{
				sleep 30;
			}
			else
			{
				http_reply($c, '200 OK', answers_for($body));
			}
		};
		close $c;
	}
}

# Answer every question id found in the request with noul 0.42.
sub answers_for
{
	my ($body) = @_;
	my %qids;

	if ($body =~ /"questions":\s*\{(.*)\}\s*\}\s*$/s)
	{
		my $qs = $1;
		$qids{$_} = 1 for $qs =~ /"(s\d+|flag)":\s*\{/g;
	}
	$qids{flag} = 1 unless %qids;

	my $answers = join(', ',
		map { "\"$_\": {\"type\": \"noul\", \"noul\": 0.42}" }
		  sort keys %qids);
	return '{"model": "jev-mock", "answers": {' . $answers . '}, '
	  . '"usage": {"input_tokens": 7, "output_tokens": 3}}';
}

sub read_http
{
	my ($c) = @_;
	my $data = '';
	while ($data !~ /\r\n\r\n/)
	{
		my $buf = '';
		my $n = $c->sysread($buf, 4096);
		last if !defined $n || $n == 0;
		$data .= $buf;
		last if length($data) > 1_000_000;
	}
	my ($headers, $body) = split(/\r\n\r\n/, $data, 2);
	$headers = '' unless defined $headers;
	$body = '' unless defined $body;
	if ($headers =~ /Content-Length:\s*(\d+)/i)
	{
		my $need = $1;
		while (length($body) < $need)
		{
			my $buf = '';
			my $n = $c->sysread($buf, $need - length($body));
			last if !defined $n || $n == 0;
			$body .= $buf;
		}
	}
	return ($headers, $body);
}

sub http_reply
{
	my ($c, $status, $body, $extra) = @_;
	$extra = '' unless defined $extra;
	my $len = length($body);
	print $c "HTTP/1.1 $status\r\n",
	  "Content-Type: application/json\r\n",
	  "Content-Length: $len\r\n",
	  $extra,
	  "Connection: close\r\n",
	  "\r\n", $body;
}
