#!/bin/bash
#Very basic regression testing does a number of regular compression /
#decompression / test cycles +/- STDIN +/- STDOUT and with the different
#compression backends.
#Run it with 
# regression_test.sh filename
#where filename is any random file to test with (big or small depending on
#what's being tested.

infile=$1

end(){
	rm -f lrztest lrztest.lrz
}

if [ ! -e $infile ]; then
        echo $infile does not exist, exiting
        exit 1
fi

if [ -f lrztest ]; then
	echo lrztest file exists, exiting
	exit 1
fi

if [ -f lrztest.lrz ]; then
	echo lrztest.lrz file exists, exiting
	exit 1
fi

trap 'echo "ABORTING";end;exit' 1 2 15

echo testing compression from stdin
./lrzip -vvlfo lrztest.lrz < $infile

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing compression from stdin
        end
	exit 1
fi
rm lrztest.lrz

echo testing compression to stdout
./lrzip -vvlo - $infile > lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing compression to stdout
        end
	exit 1
fi

rm lrztest.lrz
echo testing compression from stdin to stdout
./lrzip -vvl < $infile > lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing compression from stdin to stdout
        end
	exit 1
fi

rm lrztest.lrz
echo testing standard compression
./lrzip -vvlfo lrztest.lrz $infile

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing standard compression
        end
	exit 1
fi

echo testing standard decompression
./lrzip -vvdo lrztest lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest ];then
	echo FAILED testing standard decompression
        end
	exit 1
fi

rm lrztest
echo testing standard decompression with file checking
./lrzip -vvdfco lrztest lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest ];then
	echo FAILED testing standard decompression with file checking
        end
	exit 1
fi

rm lrztest
echo testing decompression from stdin
./lrzip -vvfo lrztest -d < lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest ];then
	echo FAILED testing decompression from stdin
        end
	exit 1
fi

rm lrztest
echo testing decompression to stdout
./lrzip -vvdo - lrztest.lrz > lrztest

if [ $? -ne 0 ] || [ ! -f lrztest ];then
	echo FAILED testing decompression to stdout
        end
	exit 1
fi

rm lrztest
echo testing decompression from stdin to stdout
./lrzip -vvd < lrztest.lrz > lrztest

if [ $? -ne 0 ] || [ ! -f lrztest ];then
	echo FAILED testing decompression from stdin to stdout
        end
	exit 1
fi

rm lrztest
echo testing testing
./lrzip -vvt lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing testing
        end
	exit 1
fi

echo testing testing from stdin
./lrzip -vvt < lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing testing from stdin
        end
	exit 1
fi

rm lrztest.lrz
echo testing rzip only compression
./lrzip -vvnfo lrztest.lrz $infile

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing rzip only compression
        end
	exit 1
fi

echo testing rzip only testing
./lrzip -vvt lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing rzip only testing
        end
	exit 1
fi

rm lrztest.lrz
echo testing lzma compression
./lrzip -vvfo lrztest.lrz $infile

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing lzma compression
        end
	exit 1
fi

echo testing lzma testing
./lrzip -vvt lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing lzma testing
        end
	exit 1
fi

rm lrztest.lrz
echo testing gzip compression
./lrzip -vvgfo lrztest.lrz $infile

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing gzip compression
        end
	exit 1
fi

echo testing gzip testing
./lrzip -vvt lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing gzip testing
        end
	exit 1
fi

rm lrztest.lrz
echo testing bzip2 compression
./lrzip -vvbfo lrztest.lrz $infile

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing bzip2 compression
        end
	exit 1
fi

echo testing bzip2 testing
./lrzip -vvt lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing bzip2 testing
        end
	exit 1
fi

rm lrztest.lrz
echo testing zpaq compression
./lrzip -vvzfo lrztest.lrz $infile

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing zpaq compression
        end
	exit 1
fi

echo testing zpaq testing
./lrzip -vvt lrztest.lrz

if [ $? -ne 0 ] || [ ! -f lrztest.lrz ];then
	echo FAILED testing zpaq testing
        end
	exit 1
fi

end

echo ALL TESTS SUCCESSFUL

exit 0
