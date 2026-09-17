# Copyright (c) 2026, Giulio Piccolo

use strict;
use warnings FATAL => 'all';

use IO::Socket::INET;
use POSIX qw(_exit);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

delete $ENV{TYPESAFE_API_KEY};

my $node = PostgreSQL::Test::Cluster->new('typesafe');
$node->init;
$node->start;

my ($ret, $stdout, $stderr) =
  $node->psql('postgres', 'CREATE EXTENSION typesafe;');
if ($ret != 0)
{
	plan skip_all => 'typesafe extension is not available';
}

# Tiny HTTP mock: require Bearer test-key, return a Choice body.
my $server = IO::Socket::INET->new(
	LocalAddr => '127.0.0.1',
	LocalPort => 0,
	Listen => 5,
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

my $endpoint = "http://127.0.0.1:$port/v1/systemone";
my $options =
  '{"billing":"Payments","technical":"Bugs","sales":"Pricing"}';
my $classify = qq{
SET typesafe.endpoint = '$endpoint';
SET typesafe.mock_response = '';
SET typesafe.timeout_ms = 5000;
SELECT choice FROM typesafe_classify(
	'Help! My payouts have been failing for 3 days.',
	'Which team should handle this?',
	'$options'::jsonb);
};

my $choice = $node->safe_psql('postgres',
	"SET typesafe.api_key = 'test-key';\n" . $classify);
is($choice, 'technical', 'HTTP classify returns technical');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"SET typesafe.api_key = 'bad-key';\n" . $classify);
isnt($ret, 0, 'bad API key fails');
like($stderr, qr/ERROR:.*401/s, 'bad API key surfaces as ERROR');

$node->stop;
done_testing();

sub mock_http
{
	my ($srv) = @_;
	my $ok =
	  '{"model":"jev-latest","answers":{"label":{"type":"choice","choice":"technical","probabilities":{"billing":0.1,"technical":0.8,"sales":0.1},"confidence":0.8}},"usage":{"input_tokens":8,"output_tokens":2}}';

	for (1 .. 8)
	{
		my $c = $srv->accept();
		next unless $c;
		$c->autoflush(1);
		eval { handle_client($c, $ok); };
		close $c;
	}
}

sub handle_client
{
	my ($c, $ok) = @_;
	my ($headers, $body) = read_http($c);

	if ($headers !~ /Authorization:\s*Bearer\s+test-key\b/i)
	{
		http_reply($c, '401 Unauthorized', '{"error":"invalid API key"}');
		return;
	}
	if ($headers !~ /^POST\b/i || $body !~ /"questions"/)
	{
		http_reply($c, '422 Unprocessable Entity', '{"error":"invalid request"}');
		return;
	}
	http_reply($c, '200 OK', $ok);
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
	my ($c, $status, $body) = @_;
	my $len = length($body);
	print $c "HTTP/1.1 $status\r\n",
	  "Content-Type: application/json\r\n",
	  "Content-Length: $len\r\n",
	  "Connection: close\r\n",
	  "\r\n", $body;
}
