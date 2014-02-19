#!/usr/bin/env ruby
# Copyright (C) 2009-2014 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

require 'time'
require 'json'
require 'pp'
require 'mongo'
require 'benchmark'
require 'ruby-prof'
require 'trollop'

def profile
  result = RubyProf.profile { yield }
  RubyProf::FlatPrinter.new(result).print(STDOUT)
  RubyProf::GraphPrinter.new(result).print(STDOUT)
end

def file_to_s(file)
  IO.read(file).chomp
end

BASE_DIR = File.expand_path('../..', __FILE__)
FULLEXPORT_DIR = "#{BASE_DIR}/ftp.musicbrainz.org/pub/musicbrainz/data/fullexport"
LATEST = "#{FULLEXPORT_DIR}/LATEST"
MBDUMP_DIR = "#{BASE_DIR}/data/fullexport/#{file_to_s(LATEST)}/mbdump"
SCHEMA_FILE = "#{BASE_DIR}/schema/create_tables.json"

$client = Mongo::MongoClient.from_uri
$db = $client['musicbrainz']
$collection = nil

$options = {}

$transform = {
    'BOOLEAN' => Proc.new {|s| if s == 't'; true; elsif s == 'f'; false; else raise 'BOOLEAN'; end },
    'CHAR(2)' => nil,
    'CHAR(3)' => nil,
    'CHAR(4)' => nil,
    'CHAR(8)' => nil,
    'CHAR(11)' => nil,
    'CHAR(12)' => nil,
    'CHAR(16)' => nil,
    'CHAR(28)' => nil,
    'CHARACTER(15)' => nil,
    'INT' => Proc.new {|s| s.to_i },
    'INTEGER' => Proc.new {|s| s.to_i },
    'SERIAL' => Proc.new {|s| s.to_i },
    'SMALLINT' => Proc.new {|s| s.to_i },
    'TEXT' => nil,
    'TIMESTAMP' => Proc.new {|s| Time.parse(s).gmtime },
    'UUID' => nil,
    'uuid' => nil,
    'VARCHAR' => nil,
    'VARCHAR(10)' => nil,
    'VARCHAR(50)' => nil,
    'VARCHAR(100)' => nil,
    'VARCHAR(255)' => nil,
    'INTEGER[]' => Proc.new {|s| s[/\{([^}]*)\}/,1].split(',').collect{|si| si.to_i} },
    'POINT' => Proc.new {|s| s[/\(([^)]*)\)/,1].split(',').collect{|si| si.to_f} }
}

def load_table(name)
  $collection = $db[name]
  $collection.remove
  create_tables = JSON.parse(IO.read(SCHEMA_FILE))
  statement = create_tables.find{|sql| sql.has_key?('create_table') && sql['create_table']['table_name'] == name}
  create_table = statement['create_table']
  table_name = create_table['table_name']
  columns = create_table['columns']
  file_name = "#{MBDUMP_DIR}/#{table_name}"
  slice_size = $options[:profile] ? 10_000 : 100_000
  IO.foreach(file_name).each_slice(slice_size) do |lines|
    bm = Benchmark.measure do
      docs = lines.collect do |line|
        values = line.chomp.split(/\t/, -1)
        zip = columns.zip(values).select{|e| e[1] != "\\N" }
        doc = zip.collect do |column, value|
          transform = $transform.fetch(column['data_type']){|key| raise "$transform[#{key.inspect} unimplemented value:#{value.inspect}"}
          value = transform.call(value) if transform
          [column['column_name'], value]
        end
        Hash[*doc.flatten(1)]
      end
      $collection.insert(docs)
    end
    puts "real:#{bm.real} docs_per_sec:#{(lines.size.to_f/bm.real).round} user:#{(100.0*bm.utime/bm.real).round}%"
    break if $options[:profile]
  end
end

banner = "usage: #{$0} [options] table_names"

$options = Trollop::options do
  banner banner
  opt :profile, "Profile", :short => 'p', :default => false
end

abort banner if ARGV.size < 1

ARGV.each do |arg|
  p arg
  if $options[:profile]
    profile { load_table(arg) }
  else
    load_table(arg)
  end
end

$client.close
