#!/bin/bash
set -e

# This script will build the training corpus, train a machine translation
#  baseline model, and evaluate it on the test set. It can be run simply
#  using:
#   nohup ./process.sh &> process.log &
#  You can follow the process of the training by using
#   tail -f process.log
#
# It requires the following tools be installed and accessible
#  For building data -->
#    KyTea:  http://www.phontron.com/kytea/  (ver. 0.2.0 or higher)
#    MT scripts:  http://homepages.inf.ed.ac.uk/jschroe1/how-to/scripts.tgz
#  For the MT system -->
#    GIZA++: http://giza-pp.googlecode.com/
#    Moses:  http://www.statmt.org/moses/
#    SRILM:  http://www.speech.sri.com/projects/srilm/download.html
# 
#  SCRIPTS_ROOTDIR must be set to the location of Moses support scripts
#  MT_SCRIPTS must be set to the directory containing the MT helper scripts
#   and the evaluation script
#
# In order to do this, you can follow the directions of Part I here:
#  http://www.statmt.org/moses_steps.html
# then install KyTea separately.
#
# If you have any problems running this, please report them to
# Graham Neubig at by sending a mail to [his last name] at gmail.com

if [[ $# != 5 ]]; then
    echo "Usage: process.sh SRC TRG PARSEMOD LABMOD DATA";
    exit 1;
fi

# Set up the values
SRC=$1
TRG=$2
PARSEMOD=$3
STIRMOD=$4
DATATYPE=$5
ID=stir-`basename $PARSEMOD .mod`;

# Set up the constants
LADER="/home/neubig/usr/arcs/local/lader/src/bin/lader"

# 0) first set the path of all variables here
WD=`pwd`
# MT_SCRIPTS=
# SCRIPTS_ROOTDIR=
# the path to KyTea
KYTEA=`which kytea` \
    && type -P $KYTEA &>/dev/null \
    || { echo "Could not find KyTea at '$KYTEA'. Aborting." >&2; exit 1; }
# the path to Moses
MOSES=`which moses` \
    && type -P $MOSES &>/dev/null \
    || { echo "Could not find Moses at '$MOSES'. Aborting." >&2; exit 1; }
# the path to ngram-count of SRILM
NGRAM_COUNT=`which ngram-count` \
    && type -P $NGRAM_COUNT &>/dev/null \
    || { echo "Could not find SRILM at '$NGRAM_COUNT'. Aborting." >&2; exit 1; }

# 4) train the language models
[[ -e lm ]] || mkdir lm
if [[ ! -e lm/kyoto-train.$TRG ]]; then
    echo "Training $TRG LM"
    $NGRAM_COUNT -order 5 -interpolate -kndiscount -text data/low/kyoto-train.cln.$TRG -lm lm/kyoto-train.$TRG
fi

# 5) reorder the data
if [[ ! -e $MOD ]]; then
    echo "ERROR: could not find specified $MOD";
    exit 1;
fi
[[ -e reordered ]] || mkdir reordered
[[ -e reordered/$MOD ]] || mkdir reordered/$MOD
if [[ ! -e reordered/$ID/kyoto-train.cln.$SRC ]]; then
    for f in kyoto-train.cln.00 kyoto-train.cln.01 kyoto-train.cln.02 kyoto-train.cln.03 kyoto-train.cln.04 kyoto-train.cln.05 kyoto-train.cln.06 kyoto-train.cln.07 kyoto-train.cln.08 kyoto-train.cln.09 kyoto-train.cln.10 kyoto-train.cln.11 kyoto-train.cln.12 kyoto-train.cln.13 kyoto-train.cln.14 kyoto-train.cln.15 kyoto-train.cln.16 kyoto-train.cln.17 kyoto-train.cln.18 kyoto-train.cln.19 kyoto-train.cln.20 kyoto-train.cln.21 kyoto-tune kyoto-dev kyoto-test; do
        rsh arcs01 "source /home/neubig/.bashrc; cd $WD; bsub '$LADER -out_format parse -beam 1 -model $PARSEMOD < data/low$DATATYPE/$f.$SRC > reordered/$ID/$f.$SRC.parse 2> reordered/$ID/$f.$SRC.parselog; script/join-features.pl data/low$DATATYPE/$f.$SRC reordered/$ID/$f.$SRC.parse > reordered/$ID/$f.$SRC.srcparse; $LADER -beam 1 -model $STIRMOD < reordered/$ID/$f.$SRC.srcparse > reordered/$ID/$f.$SRC 2> reordered/$ID/$f.$SRC.log; touch reordered/$ID/$f.$SRC.done'";
    done
    for f in kyoto-train.cln.00 kyoto-train.cln.01 kyoto-train.cln.02 kyoto-train.cln.03 kyoto-train.cln.04 kyoto-train.cln.05 kyoto-train.cln.06 kyoto-train.cln.07 kyoto-train.cln.08 kyoto-train.cln.09 kyoto-train.cln.10 kyoto-train.cln.11 kyoto-train.cln.12 kyoto-train.cln.13 kyoto-train.cln.14 kyoto-train.cln.15 kyoto-train.cln.16 kyoto-train.cln.17 kyoto-train.cln.18 kyoto-train.cln.19 kyoto-train.cln.20 kyoto-train.cln.21 kyoto-tune kyoto-dev kyoto-test; do
        while [[ ! -e reordered/$ID/$f.$SRC.done ]]; do
            echo "Waiting on $f"
            sleep 30;
        done
    done
    cat reordered/$ID/kyoto-train.cln.[012]*.$SRC > reordered/$ID/kyoto-train.cln.$SRC
    ln -s $WD/data/low/*.$TRG reordered/$ID/
fi

# 5) train moses
[[ -e experiments ]] || mkdir experiments
[[ -e experiments/$MOD ]] || mkdir experiments/$MOD
if [[ ! -e experiments/$ID/model ]]; then
    echo "Training Moses for $MOD"
    $SCRIPTS_ROOTDIR/training/train-model.perl -scripts-root-dir $SCRIPTS_ROOTDIR -root-dir experiments/$MOD -corpus $WD/reordered/$ID/kyoto-train.cln -f $SRC -e $TRG -alignment grow-diag-final-and -reordering msd-bidirectional-fe -lm 0:5:$WD/lm/kyoto-train.$TRG:0
fi

# 6) do mert
echo "Doing MERT for $MOD"
if [[ ! -e experiments/$ID/tuning ]]; then
    $SCRIPTS_ROOTDIR/training/mert-moses.pl $WD/reordered/$ID/kyoto-dev.$SRC $WD/data/low/kyoto-dev.$TRG $MOSES $WD/experiments/$ID/model/moses.ini --working-dir $WD/experiments/$ID/tuning --rootdir $SCRIPTS_ROOTDIR --decoder-flags "-v 0"  &> $WD/experiments/$ID/mert.out
fi

echo "Testing and evaluating for $MOD, test"
if [[ ! -e "experiments/$ID/evaluation/test.out" ]]; then
    # filter
    echo "$SCRIPTS_ROOTDIR/training/filter-model-given-input.pl experiments/$ID/evaluation/filtered.test experiments/$ID/tuning/moses.ini $WD/reordered/$ID/kyoto-test.$SRC"
    $SCRIPTS_ROOTDIR/training/filter-model-given-input.pl experiments/$ID/evaluation/filtered.test experiments/$ID/tuning/moses.ini $WD/reordered/$ID/kyoto-test.$SRC
    # translate
    echo "$MOSES -config experiments/$ID/evaluation/filtered.test/moses.ini < $WD/reordered/$ID/kyoto-test.$SRC 1> experiments/$ID/evaluation/test.out 2> experiments/$ID/evaluation/test.err"
$MOSES -config experiments/$ID/evaluation/filtered.test/moses.ini < $WD/reordered/$ID/kyoto-test.$SRC 1> experiments/$ID/evaluation/test.out 2> experiments/$ID/evaluation/test.err
fi

# Grade
echo "$SCRIPTS_ROOTDIR/generic/multi-bleu.perl data/low/kyoto-test.$TRG < experiments/$ID/evaluation/test.out > experiments/$ID/evaluation/test.bleu"
$SCRIPTS_ROOTDIR/generic/multi-bleu.perl data/low/kyoto-test.$TRG < experiments/$ID/evaluation/test.out > experiments/$ID/evaluation/test.bleu
echo "python3 /home/neubig/usr/local/RIBES-1.02.2/RIBES.py -r data/low/kyoto-test.$TRG experiments/$ID/evaluation/test.out > experiments/$ID/evaluation/test.ribes"
python3 /home/neubig/usr/local/RIBES-1.02.2/RIBES.py -r data/low/kyoto-test.$TRG experiments/$ID/evaluation/test.out > experiments/$ID/evaluation/test.ribes
