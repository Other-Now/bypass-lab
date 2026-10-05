# Convenience targets. The C++ build itself is plain CMake.
TF      := terraform -chdir=infra/terraform
SSH     := ssh -o StrictHostKeyChecking=accept-new
SENDER   = $(shell $(TF) output -raw sender_public_ip 2>/dev/null)

.PHONY: build test bench report aws-up aws-run aws-fetch aws-down aws-ssh

build:
	cmake -S . -B build -G Ninja
	cmake --build build

test: build
	ctest --test-dir build --output-on-failure
	python3 -m pytest -q analysis

bench: build
	./build/bl_decoder_bench data/sample.NASDAQ_ITCH50

report:
	python3 analysis/report.py $(DIR) --out $(DIR)/REPORT.md --plots $(DIR)/plots

# --- AWS (two spot instances; ~USD 0.2-0.4/hour for the pair) ----------------
# Your current public IP is the only SSH source allowed in.
aws-up:
	echo 'allowed_ssh_cidr = "'$$(curl -s https://checkip.amazonaws.com)'/32"' > infra/terraform/operator.auto.tfvars
	$(TF) init -input=false
	$(TF) apply -input=false
	bash scripts/aws/wait_ready.sh $(SENDER) $$($(TF) output -raw receiver_public_ip)

aws-run:
	$(SSH) ubuntu@$(SENDER) 'bash /opt/bypass-lab/scripts/aws/run_aws.sh'

aws-fetch:
	mkdir -p results/aws
	scp -r -o StrictHostKeyChecking=accept-new 'ubuntu@$(SENDER):/opt/bypass-lab/results/aws/*' results/aws/

aws-ssh:
	$(SSH) ubuntu@$(SENDER)

aws-down:
	$(TF) destroy -input=false -auto-approve
